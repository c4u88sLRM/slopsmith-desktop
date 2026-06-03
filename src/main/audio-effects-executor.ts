import * as fs from 'fs';
import * as path from 'path';

const PLAN_SCHEMA = 'slopsmith.audio_effects.chain_plan.v1';
const DEFAULT_ROUTE_KEY = 'desktop-main';
const MAX_STAGES = 24;
const MAX_SEGMENTS = 80;
const MAX_SEQUENTIAL_NAM = 8;
const VALID_KINDS = new Set(['nam', 'ir', 'vst', 'utility', 'bypass']);
const VALID_ROLES = new Set(['input', 'pre-pedal', 'pedal', 'amp', 'post-pedal', 'rack', 'cab', 'master-pre', 'master-post', 'utility', 'unknown']);
const AUTHORIZATIONS = new Set(['user-action', 'restore-selection', 'playback-session']);

const NATIVE_TYPES: Record<string, number> = {
    vst: 0,
    nam: 1,
    ir: 2,
};

type Dict = Record<string, unknown>;

type AudioEffectsNativeAudio = {
    loadPreset?: (presetJson: string) => Promise<unknown> | unknown;
    savePreset?: () => unknown;
    getChainState?: () => unknown;
    setBypass?: (slotId: number, bypassed: boolean) => unknown;
    setMultiBypass?: (changes: Array<{ slotId: number; bypassed: boolean }>) => unknown;
    setParameter?: (slotId: number, paramIndex: number, value: number) => unknown;
};

type NativeAudioGetter = () => AudioEffectsNativeAudio | null;

type ValidStage = {
    stageId: string;
    kind: string;
    role: string;
    assetRef: string;
    stateRef: string;
    bypassed: boolean;
    gainDb: number;
    native: boolean;
};

type ValidSegment = {
    segmentId: string;
    stageIds: string[];
};

type ValidPlan = {
    planId: string;
    routeKey: string;
    providerId: string;
    stages: ValidStage[];
    segments: ValidSegment[];
};

type RouteState = {
    routeKey: string;
    providerId: string;
    planId: string;
    state: string;
    activeSegmentId: string;
    stageSlots: Map<string, number>;
    stageKinds: Map<string, string>;
    segments: ValidSegment[];
    loadedAt: string;
    updatedAt: string;
    lastOutcome: SafeOutcome | null;
};

type SafeOutcome = {
    outcome: string;
    status: string;
    reason: string;
    payload?: Dict;
};

function asRecord(value: unknown): Dict | null {
    return value && typeof value === 'object' && !Array.isArray(value) ? value as Dict : null;
}

function asArray(value: unknown): unknown[] {
    return Array.isArray(value) ? value : [];
}

function now(): string {
    return new Date().toISOString();
}

function bounded(value: unknown, max = 200): string {
    return String(value ?? '')
        .replace(/(?:\/Users\/|\/home\/|\/root\b\/?)[^\r\n\t"'`,;(){}\[\]<>|]*/g, '[path]')
        .replace(/[A-Za-z]:\\[^\r\n\t"'`,;(){}\[\]<>|]*/g, '[path]')
        .replace(/https?:\/\/[^\s?#]+[^\s]*/gi, '[url]')
        .replace(/file:\/\/[^\s]+/gi, '[path]')
        .replace(/\b(token|secret|password|api[_-]?key|key)=([^\s&]+)/gi, '$1=[redacted]')
        .replace(/\b[^\s]+\.(psarc|sloppak|wem|ogg|mp3|wav|flac|nam|vst3|component|dll|json|db)\b/gi, '[file]')
        .replace(/\s+/g, ' ')
        .trim()
        .slice(0, max);
}

function safeId(value: unknown, fallback: string): string {
    const text = String(value ?? '').trim() || fallback;
    return text.replace(/[^A-Za-z0-9_.:-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 96) || fallback;
}

function safeNumber(value: unknown, fallback = 0): number {
    const numberValue = Number(value);
    return Number.isFinite(numberValue) ? numberValue : fallback;
}

function safeBool(value: unknown, fallback = false): boolean {
    return value === true || value === false ? value : fallback;
}

function safeOutcome(outcome: string, reason: unknown, payload?: Dict, status?: string): SafeOutcome {
    return {
        outcome,
        status: status || outcome,
        reason: bounded(reason),
        ...(payload ? { payload } : {}),
    };
}

function safeRoute(route: RouteState): Dict {
    return {
        routeKey: route.routeKey,
        providerId: route.providerId,
        planId: route.planId,
        state: route.state,
        activeSegmentId: route.activeSegmentId,
        nativeStageCount: route.stageSlots.size,
        stageKinds: Array.from(route.stageKinds.values()),
        segmentCount: route.segments.length,
        loadedAt: route.loadedAt,
        updatedAt: route.updatedAt,
        lastOutcome: route.lastOutcome ? {
            outcome: route.lastOutcome.outcome,
            status: route.lastOutcome.status,
            reason: route.lastOutcome.reason,
        } : null,
    };
}

function normalizeAssetMap(value: unknown): Map<string, Dict> {
    const map = new Map<string, Dict>();
    const record = asRecord(value);
    if (!record) return map;
    for (const [key, entry] of Object.entries(record)) {
        const asset = asRecord(entry);
        if (asset) map.set(key, asset);
    }
    return map;
}

function validateAssetPath(filePath: unknown, kind: string, errors: string[], stageId: string): string {
    const candidate = String(filePath ?? '').trim();
    if (!candidate) {
        errors.push(`Stage ${stageId} has no trusted asset path`);
        return '';
    }
    if (/^(?:https?:|file:)/i.test(candidate) || !path.isAbsolute(candidate)) {
        errors.push(`Stage ${stageId} asset path must be an absolute local path inside the trusted executor call`);
        return '';
    }
    const ext = path.extname(candidate).toLowerCase();
    const validExt = kind === 'nam'
        ? ext === '.nam'
        : kind === 'ir'
            ? ['.wav', '.flac', '.aiff', '.aif'].includes(ext)
            : kind === 'vst'
                ? ['.vst3', '.component', '.dll'].includes(ext)
                : true;
    if (!validExt) errors.push(`Stage ${stageId} asset extension is not valid for ${kind}`);
    if (!fs.existsSync(candidate)) errors.push(`Stage ${stageId} trusted asset is missing`);
    return candidate;
}

function validatePlan(request: unknown): { ok: true; plan: ValidPlan; presetJson: string } | { ok: false; errors: string[] } {
    const input = asRecord(request) || {};
    const planInput = asRecord(input.plan) || asRecord(input.chainPlan) || null;
    const errors: string[] = [];
    if (!planInput) return { ok: false, errors: ['Missing audio-effects chain plan'] };

    const authorization = String(input.authorization ?? '').trim();
    if (!AUTHORIZATIONS.has(authorization)) {
        errors.push('Audio-effects plan loading requires user-action, restore-selection, or playback-session authorization');
    }

    const schema = String(planInput.schema ?? '').trim();
    if (schema !== PLAN_SCHEMA) errors.push('Unsupported audio-effects chain plan schema');

    const routeKey = safeId(planInput.routeKey ?? planInput.route ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
    const providerId = safeId(planInput.providerId, 'provider');
    const planId = safeId(planInput.planId ?? planInput.chainId, 'plan');
    const rawStages = asArray(planInput.stages);
    if (rawStages.length < 1) errors.push('Audio-effects chain plan must include at least one stage');
    if (rawStages.length > MAX_STAGES) errors.push(`Audio-effects chain plan exceeds maximum stage count ${MAX_STAGES}`);

    const assets = normalizeAssetMap(input.assets ?? input.trustedAssets);
    const states = normalizeAssetMap(input.states ?? input.trustedStates);
    const stages: ValidStage[] = [];
    const nativePresetChain: Dict[] = [];
    let sequentialNam = 0;

    rawStages.slice(0, MAX_STAGES).forEach((entry, index) => {
        const stageInput = asRecord(entry) || {};
        const kind = safeId(stageInput.kind, 'utility');
        const roleCandidate = safeId(stageInput.role ?? stageInput.slot, 'unknown');
        const role = VALID_ROLES.has(roleCandidate) ? roleCandidate : 'unknown';
        const stageId = safeId(stageInput.stageId ?? stageInput.id ?? `${kind}-${index}`, `stage-${index}`);
        const assetRef = String(stageInput.assetRef ?? stageInput.ref ?? '').trim();
        const stateRef = String(stageInput.stateRef ?? '').trim();
        const native = kind === 'nam' || kind === 'ir' || kind === 'vst';

        if (!VALID_KINDS.has(kind)) errors.push(`Stage ${stageId} has unsupported kind`);
        if (native && !assetRef) errors.push(`Stage ${stageId} requires an opaque assetRef`);
        if (kind === 'nam') sequentialNam += 1;
        else sequentialNam = 0;
        if (sequentialNam > MAX_SEQUENTIAL_NAM) errors.push(`Plan exceeds maximum sequential NAM count ${MAX_SEQUENTIAL_NAM}`);

        const stage: ValidStage = {
            stageId,
            kind: VALID_KINDS.has(kind) ? kind : 'utility',
            role,
            assetRef,
            stateRef,
            bypassed: safeBool(stageInput.bypassed, false),
            gainDb: safeNumber(stageInput.gainDb, 0),
            native,
        };
        stages.push(stage);

        if (!native) return;

        const asset = assets.get(assetRef);
        if (!asset) {
            errors.push(`Stage ${stageId} has no trusted asset for its assetRef`);
            return;
        }
        const assetKind = safeId(asset.kind, kind);
        if (assetKind !== kind) errors.push(`Stage ${stageId} trusted asset kind does not match plan kind`);
        const assetPath = validateAssetPath(asset.path, kind, errors, stageId);
        const state = stateRef ? states.get(stateRef) : null;
        const stateBase64 = String(asset.stateBase64 ?? state?.stateBase64 ?? state?.base64 ?? '').trim();
        const nativeStage: Dict = {
            type: NATIVE_TYPES[kind],
            name: bounded(asset.safeName ?? asset.label ?? `${role}-${kind}`, 96) || `${role}-${kind}`,
            path: assetPath,
            bypassed: stage.bypassed,
        };
        if (stateBase64) nativeStage.state = stateBase64;
        nativePresetChain.push(nativeStage);
    });

    const nativeStageIds = stages.filter((stage) => stage.native).map((stage) => stage.stageId);
    const segments: ValidSegment[] = asArray(planInput.segments).slice(0, MAX_SEGMENTS).map((segment, index) => {
        const item = asRecord(segment) || {};
        return {
            segmentId: safeId(item.segmentId ?? item.toneKey ?? item.id ?? `segment-${index}`, `segment-${index}`),
            stageIds: asArray(item.stageIds ?? item.stages).map((value) => safeId(value, '')).filter((value) => nativeStageIds.includes(value)),
        };
    });

    if (nativePresetChain.length < 1) errors.push('Audio-effects chain plan has no loadable native stages');

    if (errors.length) return { ok: false, errors };
    return {
        ok: true,
        plan: { planId, routeKey, providerId, stages, segments },
        presetJson: JSON.stringify({ chain: nativePresetChain }),
    };
}

function normalizeLoadResult(value: unknown): { success: boolean; slotsLoaded: number; error: string } {
    const record = asRecord(value);
    if (!record) return { success: false, slotsLoaded: 0, error: 'Native load returned an unsupported result' };
    return {
        success: record.success === true,
        slotsLoaded: safeNumber(record.slotsLoaded, 0),
        error: bounded(record.error ?? ''),
    };
}

function chainSlots(nativeAudio: AudioEffectsNativeAudio | null): Dict[] {
    if (!nativeAudio || typeof nativeAudio.getChainState !== 'function') return [];
    const state = nativeAudio.getChainState();
    return asArray(state).map((entry) => asRecord(entry)).filter((entry): entry is Dict => !!entry);
}

async function restorePreset(nativeAudio: AudioEffectsNativeAudio, presetJson: unknown): Promise<boolean> {
    if (typeof presetJson !== 'string' || !presetJson.trim() || typeof nativeAudio.loadPreset !== 'function') return false;
    try {
        await nativeAudio.loadPreset(presetJson);
        return true;
    } catch (_) {
        return false;
    }
}

export function createAudioEffectsExecutor(getAudio: NativeAudioGetter) {
    const routes = new Map<string, RouteState>();

    function updateOutcome(route: RouteState, outcome: SafeOutcome): SafeOutcome {
        route.lastOutcome = outcome;
        route.updatedAt = now();
        return outcome;
    }

    async function loadChainPlan(request: unknown): Promise<SafeOutcome> {
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.loadPreset !== 'function') {
            return safeOutcome('unavailable', 'Native audio engine is unavailable');
        }
        const validation = validatePlan(request);
        if (!validation.ok) {
            return safeOutcome('failed', 'Audio-effects chain plan validation failed', { errors: validation.errors.map((error) => bounded(error)) });
        }

        const started = Date.now();
        const rollbackPreset = typeof nativeAudio.savePreset === 'function' ? nativeAudio.savePreset() : null;
        let result: { success: boolean; slotsLoaded: number; error: string };
        try {
            result = normalizeLoadResult(await nativeAudio.loadPreset(validation.presetJson));
        } catch (error) {
            await restorePreset(nativeAudio, rollbackPreset);
            return safeOutcome('failed', 'Native audio-effects plan load threw', { error: bounded(error instanceof Error ? error.message : String(error)) });
        }

        const nativeStages = validation.plan.stages.filter((stage) => stage.native);
        if (!result.success) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            return safeOutcome('failed', 'Native audio-effects plan load failed', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                stageCount: nativeStages.length,
                slotsLoaded: result.slotsLoaded,
                loadMs: Date.now() - started,
                error: result.error,
                rollbackApplied,
            });
        }

        if (result.slotsLoaded < nativeStages.length) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            return safeOutcome('degraded', 'Native audio-effects plan partially loaded and was rolled back', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                stageCount: nativeStages.length,
                slotsLoaded: result.slotsLoaded,
                loadMs: Date.now() - started,
                rollbackApplied,
            });
        }

        const slots = chainSlots(nativeAudio);
        const stageSlots = new Map<string, number>();
        const stageKinds = new Map<string, string>();
        nativeStages.forEach((stage, index) => {
            const slotId = safeNumber(slots[index]?.id, -1);
            if (slotId >= 0) stageSlots.set(stage.stageId, slotId);
            stageKinds.set(stage.stageId, stage.kind);
        });

        const route: RouteState = {
            routeKey: validation.plan.routeKey,
            providerId: validation.plan.providerId,
            planId: validation.plan.planId,
            state: result.slotsLoaded >= nativeStages.length ? 'loaded' : 'degraded',
            activeSegmentId: '',
            stageSlots,
            stageKinds,
            segments: validation.plan.segments,
            loadedAt: now(),
            updatedAt: now(),
            lastOutcome: null,
        };
        routes.set(route.routeKey, route);
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects chain plan loaded', {
            route: safeRoute(route),
            stageCount: nativeStages.length,
            slotsLoaded: result.slotsLoaded,
            loadMs: Date.now() - started,
        }));
    }

    function inspectRoute(routeKeyInput?: unknown): SafeOutcome {
        const routeKey = safeId(routeKeyInput ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        return safeOutcome('handled', 'Audio-effects route inspected', { route: safeRoute(route) });
    }

    function setStageBypass(request: unknown): SafeOutcome {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const stageId = safeId(input.stageId, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const slotId = route.stageSlots.get(stageId);
        if (slotId == null) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects stage is not mapped to a native slot', { routeKey, stageId }));
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setBypass !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native stage bypass is unavailable', { routeKey, stageId }));
        nativeAudio.setBypass(slotId, safeBool(input.bypassed, false));
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects stage bypass applied', { route: safeRoute(route), stageId }));
    }

    function setStageParameter(request: unknown): SafeOutcome {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const stageId = safeId(input.stageId, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const slotId = route.stageSlots.get(stageId);
        if (slotId == null) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects stage is not mapped to a native slot', { routeKey, stageId }));
        const paramIndex = Number.isFinite(Number(input.paramIndex))
            ? Number(input.paramIndex)
            : Number(input.parameterId);
        const value = Number(input.value);
        if (!Number.isInteger(paramIndex) || paramIndex < 0 || !Number.isFinite(value)) {
            return updateOutcome(route, safeOutcome('failed', 'Audio-effects stage parameter request is invalid', { routeKey, stageId }));
        }
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setParameter !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native stage parameter control is unavailable', { routeKey, stageId }));
        nativeAudio.setParameter(slotId, paramIndex, value);
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects stage parameter applied', { route: safeRoute(route), stageId, paramIndex }));
    }

    function activateSegment(request: unknown): SafeOutcome {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const segmentId = safeId(input.segmentId ?? input.toneKey, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const segment = route.segments.find((item) => item.segmentId === segmentId);
        if (!segment) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects segment is not present in the loaded plan', { routeKey, segmentId }));
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setMultiBypass !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native multi-bypass is unavailable', { routeKey, segmentId }));
        const active = new Set(segment.stageIds);
        const changes = Array.from(route.stageSlots.entries()).map(([stageId, slotId]) => ({
            slotId,
            bypassed: !active.has(stageId),
        }));
        nativeAudio.setMultiBypass(changes);
        route.activeSegmentId = segment.segmentId;
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects segment activated', { route: safeRoute(route), segmentId, changedCount: changes.length }));
    }

    return {
        loadChainPlan,
        inspectRoute,
        activateSegment,
        setStageBypass,
        setStageParameter,
    };
}

export type AudioEffectsExecutor = ReturnType<typeof createAudioEffectsExecutor>;
