export type NodeType = "input" | "output" | "nam" | "ir" | "split" | "merge" | "bypass" | "fx";

export interface NodeParams {
  levelDb: number;
  mix: number;
  slim: number;
  bypass: boolean;
  filePath: string;
  displayName: string;
  toneId: string;
  modelId: string;
  imageUrl?: string;
  cabIncluded?: boolean;
  inputChannel?: number;
  pan?: number;
  outputChannel?: number;
  outputChannelR?: number;
  bassDb?: number;
  midDb?: number;
  trebleDb?: number;
  roomSize?: number;
  damping?: number;
  width?: number;
  delayMs?: number;
  feedback?: number;
  thresholdDb?: number;
  attackMs?: number;
  releaseMs?: number;
  gateRangeDb?: number;
  hysteresisDb?: number;
  holdMs?: number;
  ratio?: number;
  makeupDb?: number;
  rateHz?: number;
  depth?: number;
  fxMode?: string;
  fxId?: string;
}

export interface GraphNode {
  id: string;
  type: NodeType;
  x: number;
  y: number;
  params: NodeParams;
}

export interface GraphEdge {
  id: string;
  fromNode: string;
  fromPort: number;
  toNode: string;
  toPort: number;
}

export interface GraphDocument {
  version: number;
  name: string;
  nodes: GraphNode[];
  edges: GraphEdge[];
}

export interface LibraryItem {
  id: string;
  kind: "nam" | "ir" | "fx" | "routing" | "folder";
  name: string;
  filePath?: string;
  toneId?: string;
  modelId?: string;
  format?: string;
  source?: string;
  notes?: string;
  imageUrl?: string;
  cabIncluded?: boolean;
}

export interface IoInfo {
  numInputs: number;
  numOutputs: number;
  inputPeak: number;
  outputPeak: number;
  inputPeakL?: number;
  inputPeakR?: number;
  outputPeakL?: number;
  outputPeakR?: number;
  activeInputChannel: number;
  activeOutputChannel?: number;
  inputMuted?: boolean;
  isStandalone?: boolean;
  buildId?: string;
  masterInDb?: number;
  masterOutDb?: number;
  masterDb?: number;
  clipping?: boolean;
}

type NativeResult = { ok: boolean; data?: unknown; error?: string };

type JuceBackend = {
  emitEvent: (id: string, payload: unknown) => void;
  addEventListener: (id: string, cb: (payload: unknown) => void) => void;
};

function getBackend(): JuceBackend | null {
  return (window as unknown as { __JUCE__?: { backend?: JuceBackend } }).__JUCE__?.backend ?? null;
}

let promiseIdCounter = 0;
const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: unknown) => void }>();
let listenerReady = false;

function ensureCompleteListener() {
  if (listenerReady) return;
  const backend = getBackend();
  if (!backend) return;
  backend.addEventListener("__juce__complete", (payload: unknown) => {
    const p = payload as { promiseId?: number; result?: unknown };
    if (p?.promiseId == null) return;
    const entry = pending.get(p.promiseId);
    if (!entry) return;
    pending.delete(p.promiseId);
    entry.resolve(p.result);
  });
  listenerReady = true;
}

function getNative(name: string): ((...args: unknown[]) => Promise<unknown>) | null {
  const w = window as unknown as {
    getNativeFunction?: (n: string) => (...args: unknown[]) => Promise<unknown>;
  };
  if (typeof w.getNativeFunction === "function") return w.getNativeFunction(name);

  const backend = getBackend();
  if (!backend) return null;
  ensureCompleteListener();

  return (...args: unknown[]) => {
    const promiseId = promiseIdCounter++;
    const result = new Promise((resolve, reject) => pending.set(promiseId, { resolve, reject }));
    backend.emitEvent("__juce__invoke", {
      name,
      params: args,
      resultId: promiseId,
    });
    return result;
  };
}

async function call(method: string, args?: unknown): Promise<NativeResult> {
  const fn = getNative(method);
  if (!fn) {
    if (method === "getGraph") return { ok: true, data: demoGraph() };
    if (method === "getLibrary") return { ok: true, data: demoLibrary() };
    if (method === "listPresets") return { ok: true, data: ["Empty", "Amp Cab"] };
    if (method === "toneStatus") return { ok: true, data: { signedIn: false, hasKey: false } };
    if (method === "getCpu") return { ok: true, data: 0.05 };
    if (method === "getIoInfo")
      return {
        ok: true,
        data: {
          numInputs: 8,
          numOutputs: 2,
          inputPeak: 0.1,
          outputPeak: 0.05,
          inputPeakL: 0.1,
          inputPeakR: 0.08,
          outputPeakL: 0.05,
          outputPeakR: 0.04,
          activeInputChannel: 0,
          activeOutputChannel: 0,
          isStandalone: true,
        },
      };
    return { ok: false, error: "Native bridge unavailable (open inside Namplifier)" };
  }
  const raw = await fn(args ?? {});
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw) as NativeResult;
    } catch {
      return { ok: false, error: raw };
    }
  }
  return raw as NativeResult;
}

function demoLibrary(): LibraryItem[] {
  return [
    {
      id: "d1",
      kind: "nam",
      name: "Demo Plexi",
      source: "local",
      format: "nam",
      imageUrl: "https://placehold.co/64x64/1a1a1a/666?text=NAM",
    },
    { id: "d2", kind: "ir", name: "Demo Cab", source: "local", format: "ir" },
    { id: "fx1", kind: "fx", name: "Reverb", format: "reverb", source: "factory", notes: "Stereo room" },
    { id: "fx2", kind: "fx", name: "Delay", format: "delay", source: "factory", notes: "Stereo delay" },
    { id: "fx3", kind: "fx", name: "Ping Pong Delay", format: "pingpong", source: "factory", notes: "L↔R bounce" },
    { id: "fxg", kind: "fx", name: "Noise Gate", format: "gate", source: "factory", notes: "Before amp" },
    { id: "fx3", kind: "fx", name: "Chorus (soon)", source: "factory", notes: "Coming later" },
    { id: "r1", kind: "routing", name: "Input", format: "input", source: "factory", notes: "Audio input" },
    { id: "r2", kind: "routing", name: "Split", format: "split", source: "factory", notes: "Fan-out" },
    { id: "r3", kind: "routing", name: "Merge", format: "merge", source: "factory", notes: "Blend paths" },
    { id: "r4", kind: "routing", name: "Output", format: "output", source: "factory", notes: "Stereo out" },
  ];
}

function demoGraph(): GraphDocument {
  return {
    version: 1,
    name: "Amp -> Cab",
    nodes: [
      { id: "input", type: "input", x: 60, y: 200, params: { ...emptyParams(), inputChannel: 0 } },
      { id: "nam1", type: "nam", x: 220, y: 200, params: { ...emptyParams(), displayName: "NAM Amp" } },
      { id: "ir1", type: "ir", x: 400, y: 200, params: { ...emptyParams(), displayName: "IR Cab" } },
      { id: "output", type: "output", x: 580, y: 200, params: emptyParams() },
    ],
    edges: [
      { id: "e1", fromNode: "input", fromPort: 0, toNode: "nam1", toPort: 0 },
      { id: "e2", fromNode: "nam1", fromPort: 0, toNode: "ir1", toPort: 0 },
      { id: "e3", fromNode: "ir1", fromPort: 0, toNode: "output", toPort: 0 },
    ],
  };
}

export function emptyParams(): NodeParams {
  return {
    levelDb: 0,
    mix: 1,
    slim: 1,
    bypass: false,
    filePath: "",
    displayName: "",
    toneId: "",
    modelId: "",
    imageUrl: "",
    cabIncluded: false,
    inputChannel: 0,
    pan: 0,
    outputChannel: 0,
    outputChannelR: 1,
    bassDb: 0,
    midDb: 0,
    trebleDb: 0,
    roomSize: 0.45,
    damping: 0.4,
    width: 1,
    delayMs: 350,
    feedback: 0.35,
    thresholdDb: -50,
    attackMs: 2,
    releaseMs: 150,
    gateRangeDb: -70,
    hysteresisDb: 8,
    holdMs: 80,
    ratio: 4,
    makeupDb: 0,
    rateHz: 0.8,
    depth: 0.35,
    fxMode: "",
    fxId: "",
  };
}

export const native = {
  getGraph: () => call("getGraph"),
  setGraph: (g: GraphDocument) => call("setGraph", g),
  updateNode: (id: string, params: NodeParams) => call("updateNode", { id, params }),
  pickFileForNode: (id: string, type: string) => call("pickFileForNode", { id, type }),
  getCpu: () => call("getCpu"),
  getIoInfo: () => call("getIoInfo"),
  getDspStatus: () => call("getDspStatus"),
  getMaster: () => call("getMaster"),
  setMaster: (inputDb?: number, outputDb?: number) => {
    const args: Record<string, unknown> = {};
    if (inputDb != null && Number.isFinite(inputDb)) args.inputDb = inputDb;
    if (outputDb != null && Number.isFinite(outputDb)) args.outputDb = outputDb;
    return call("setMaster", args);
  },
  unmuteInput: () => call("unmuteInput"),
  toggleFullscreen: () => call("toggleFullscreen"),
  listPresets: () => call("listPresets"),
  savePreset: (name: string) => call("savePreset", { name }),
  loadPreset: (name: string) => call("loadPreset", { name }),
  deletePreset: (name: string) => call("deletePreset", { name }),
  toneStatus: () => call("toneStatus"),
  toneBeginLogin: () => call("toneBeginLogin"),
  toneCompleteLogin: (code: string, state: string) => call("toneCompleteLogin", { code, state }),
  toneLogout: () => call("toneLogout"),
  toneSearch: (opts: {
    query?: string;
    format?: string;
    architecture?: string;
    sort?: string;
    gears?: string;
    sizes?: string;
    calibrated?: boolean;
    page?: number;
    pageSize?: number;
  }) =>
    call("toneSearch", {
      query: opts.query ?? "",
      format: opts.format ?? "",
      architecture: opts.architecture ?? "",
      sort: opts.sort ?? "",
      gears: opts.gears ?? "",
      sizes: opts.sizes ?? "",
      calibrated: Boolean(opts.calibrated),
      page: opts.page ?? 1,
      pageSize: opts.pageSize ?? 20,
    }),
  toneFavorites: (page = 1, pageSize = 25) => call("toneFavorites", { page, pageSize }),
  toneCreated: (page = 1, pageSize = 25) => call("toneCreated", { page, pageSize }),
  toneDownloaded: (page = 1, pageSize = 25) => call("toneDownloaded", { page, pageSize }),
  toneFavorite: (toneId: string, favorite: boolean) => call("toneFavorite", { toneId, favorite }),
  toneGetModels: (toneId: string, format: string) => call("toneGetModels", { toneId, format }),
  toneLoadToNode: (
    nodeId: string,
    toneId: string,
    format?: string,
    modelId?: string,
    name?: string,
    imageUrl?: string,
    cabIncluded?: boolean
  ) =>
    call("toneLoadToNode", {
      nodeId,
      toneId,
      format: format ?? "",
      modelId: modelId ?? "",
      name: name ?? "",
      imageUrl: imageUrl ?? "",
      cabIncluded: Boolean(cabIncluded),
    }),
  toneSetNodeModel: (
    nodeId: string,
    toneId: string,
    modelId: string,
    format?: string,
    name?: string,
    imageUrl?: string,
    cabIncluded?: boolean
  ) =>
    call("toneSetNodeModel", {
      nodeId,
      toneId,
      modelId,
      format: format ?? "",
      name: name ?? "",
      imageUrl: imageUrl ?? "",
      cabIncluded: Boolean(cabIncluded),
    }),
  toneAddToLibrary: (
    toneId: string,
    name: string,
    format: string,
    imageUrl?: string,
    cabIncluded?: boolean,
    modelId?: string
  ) =>
    call("toneAddToLibrary", {
      toneId,
      name,
      format,
      imageUrl: imageUrl ?? "",
      cabIncluded: Boolean(cabIncluded),
      modelId: modelId ?? "",
    }),
  toneAddToGraph: (
    toneId: string,
    name: string,
    format: string,
    imageUrl?: string,
    cabIncluded?: boolean,
    x?: number,
    y?: number,
    modelId?: string
  ) => {
    const args: Record<string, unknown> = {
      toneId,
      name,
      format,
      imageUrl: imageUrl ?? "",
      cabIncluded: Boolean(cabIncluded),
      modelId: modelId ?? "",
    };
    if (x != null && y != null && Number.isFinite(x) && Number.isFinite(y)) {
      args.x = x;
      args.y = y;
    }
    return call("toneAddToGraph", args);
  },
  getLibrary: () => call("getLibrary"),
  libraryRemove: (id: string) => call("libraryRemove", { id }),
  libraryAddLocal: (kind: string) => call("libraryAddLocal", { kind }),
  libraryApplyToNode: (nodeId: string, libraryId: string) =>
    call("libraryApplyToNode", { nodeId, libraryId }),
  libraryAddToGraph: (libraryId: string, x?: number, y?: number) => {
    const args: Record<string, unknown> = { libraryId };
    if (x != null && y != null && Number.isFinite(x) && Number.isFinite(y)) {
      args.x = x;
      args.y = y;
    }
    return call("libraryAddToGraph", args);
  },
  openExternal: (url: string) => call("openExternal", { url }),
};

export function onNativeEvent(id: string, cb: () => void) {
  const backend = getBackend();
  backend?.addEventListener(id, () => cb());
}

declare global {
  interface Window {
    __JUCE__?: {
      backend?: {
        emitEvent?: (id: string, payload: unknown) => void;
        addEventListener?: (id: string, cb: (payload: unknown) => void) => void;
      };
    };
  }
}
