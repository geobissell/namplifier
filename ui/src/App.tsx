import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  GraphDocument,
  GraphEdge,
  GraphNode,
  IoInfo,
  LibraryItem,
  NodeParams,
  NodeType,
  emptyParams,
  native,
  onNativeEvent,
} from "./bridge";
import tone3000Logo from "./assets/tone3000-logo.svg";

type ToneHit = {
  id: string;
  name: string;
  creator?: string;
  creatorAvatar?: string;
  format?: string;
  gear?: string;
  description?: string;
  imageUrl?: string;
  downloads?: number;
  favorites?: number;
  modelsCount?: number;
  gearType?: string;
  cabIncluded?: boolean;
  favorited?: boolean;
};
type LibFilter = "all" | "nam" | "ir" | "fx" | "routing";

function uid(prefix: string) {
  return `${prefix}_${Math.random().toString(36).slice(2, 9)}`;
}

function nodeLabel(n: GraphNode) {
  if (n.params.displayName) return n.params.displayName;
  if (n.type === "input") return "Input";
  if (n.type === "output") return "Output";
  if (n.type === "split") return "Split";
  if (n.type === "merge") return "Merge";
  if (n.type === "media") return "Media File";
  if (n.type === "youtube") return "YouTube";
  return n.type.toUpperCase();
}

const NODE_W = 176;

function isStereoFxNode(n: GraphNode) {
  if (n.type !== "fx") return false;
  const id = (n.params.fxId || "").toLowerCase();
  const name = (n.params.displayName || "").toLowerCase();
  return (
    id === "reverb" ||
    id === "delay" ||
    id === "pingpong" ||
    id === "chorus" ||
    name.includes("reverb") ||
    name.includes("delay") ||
    name.includes("ping") ||
    name.includes("chorus")
  );
}

function numInPorts(n: GraphNode) {
  if (n.type === "input" || n.type === "media" || n.type === "youtube") return 0;
  if (n.type === "merge") return 2;
  // Output / Host Output: one stereo bus in (wire once; L/R come from the source)
  if (n.type === "output") return 1;
  if (isStereoFxNode(n)) return 2;
  return 1; // split, nam, ir, gate, …
}

function numOutPorts(n: GraphNode) {
  if (n.type === "output") return 0;
  if (n.type === "split") return 2; // A / B
  if (isStereoFxNode(n)) return 2;
  return 1;
}

function isHostOutput(n: GraphNode) {
  return n.type === "output" && (n.params.fxId === "host_out" || (n.params.displayName || "").toLowerCase().includes("host"));
}

function portLabel(n: GraphNode, dir: "in" | "out", index: number) {
  if (n.type === "split" && dir === "out") return index === 0 ? "A" : "B";
  if (n.type === "merge" && dir === "in") return index === 0 ? "A" : "B";
  if (isStereoFxNode(n) && dir === "in") return index === 0 ? "L" : "R";
  if (isStereoFxNode(n) && dir === "out") return index === 0 ? "L" : "R";
  return "";
}

function portPos(n: GraphNode, dir: "in" | "out", portIndex = 0) {
  const count = dir === "in" ? numInPorts(n) : numOutPorts(n);
  const span = Math.max(1, count);
  const y = n.y + 22 + ((portIndex + 0.5) / span) * Math.max(44, span * 22);
  // Ports sit outside the body (CSS left/right: -20px, 14px wide → center ~13px out)
  // Match CSS: 14px ports at left/right: -7px → center sits ~0px outside the edge
  return { x: dir === "in" ? n.x : n.x + NODE_W, y };
}

/** Reassign ports by order only when every cable shares the same port (legacy graphs). */
function migratePortGroup(
  edges: GraphEdge[],
  nodeId: string,
  side: "from" | "to",
  maxPort: number
) {
  const portKey = side === "from" ? "fromPort" : "toPort";
  const idKey = side === "from" ? "fromNode" : "toNode";
  const list = edges.filter((e) => e[idKey] === nodeId);
  if (list.length === 0) return;
  const ports = list.map((e) => Math.max(0, e[portKey] ?? 0));
  const allSame = ports.every((p) => p === ports[0]);
  if (allSame && list.length > 1) {
    list.forEach((e, i) => {
      e[portKey] = Math.min(i, maxPort);
    });
  } else {
    for (const e of list) {
      e[portKey] = Math.max(0, Math.min(maxPort, e[portKey] ?? 0));
    }
  }
}

/** Ensure one cable per port; migrate legacy multi-cable Split/Merge/FX wiring. */
function normalizeGraphPorts(g: GraphDocument): GraphDocument {
  const edges = g.edges.map((e) => ({ ...e }));
  const byId = new Map(g.nodes.map((n) => [n.id, n]));

  for (const n of g.nodes) {
    const outs = Math.max(0, numOutPorts(n) - 1);
    const inns = Math.max(0, numInPorts(n) - 1);
    if (numOutPorts(n) > 1) migratePortGroup(edges, n.id, "from", outs);
    if (numInPorts(n) > 1) migratePortGroup(edges, n.id, "to", inns);
  }

  // Enforce one edge per (node, port, direction) — keep last write order
  const usedOut = new Set<string>();
  const usedIn = new Set<string>();
  const kept: GraphEdge[] = [];
  for (const e of edges) {
    const from = byId.get(e.fromNode);
    const to = byId.get(e.toNode);
    if (!from || !to) continue;
    const maxOut = Math.max(0, numOutPorts(from) - 1);
    const maxIn = Math.max(0, numInPorts(to) - 1);
    const fp = Math.max(0, Math.min(maxOut, e.fromPort ?? 0));
    const tp = Math.max(0, Math.min(maxIn, e.toPort ?? 0));
    if (numOutPorts(from) <= 0 || numInPorts(to) <= 0) continue;
    const ok = `${e.fromNode}:${fp}`;
    const ik = `${e.toNode}:${tp}`;
    if (usedOut.has(ok) || usedIn.has(ik)) continue;
    usedOut.add(ok);
    usedIn.add(ik);
    kept.push({ ...e, fromPort: fp, toPort: tp });
  }
  return { ...g, edges: kept };
}

function isReadyFx(item: { kind: string; format?: string; name: string }) {
  if (item.kind !== "fx") return false;
  const f = (item.format || "").toLowerCase();
  const n = item.name.toLowerCase();
  if (n.includes("soon") || n.includes("flanger")) return false;
  return (
    f === "reverb" ||
    f === "delay" ||
    f === "pingpong" ||
    f === "gate" ||
    f === "chorus" ||
    f === "compressor" ||
    f === "comp" ||
    n === "reverb" ||
    n === "delay" ||
    n.includes("ping") ||
    n.includes("gate") ||
    n.includes("chorus") ||
    n.includes("compress")
  );
}

function isRoutingItem(item: { kind: string }) {
  return item.kind === "routing";
}

function peakToDb(peak: number) {
  if (peak <= 1e-6) return -60;
  return Math.max(-60, 20 * Math.log10(peak));
}

function Meter({ label, peak }: { label: string; peak: number }) {
  const db = peakToDb(peak);
  const pct = Math.min(100, ((db + 60) / 60) * 100);
  return (
    <div className="meter">
      <span>{label}</span>
      <div className="meter-track">
        <div className="meter-fill" style={{ width: `${pct}%` }} />
      </div>
      <small>{db <= -59 ? "−∞" : `${db.toFixed(0)} dB`}</small>
    </div>
  );
}

/** Slider + editable number for inspector params */
function ParamNum({
  label,
  value,
  min,
  max,
  step = 0.01,
  decimals,
  suffix = "",
  title,
  onChange,
  onReset,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  decimals?: number;
  suffix?: string;
  title?: string;
  onChange: (v: number) => void;
  onReset?: () => void;
}) {
  const places = decimals ?? (step < 1 ? (step < 0.1 ? 2 : 1) : 0);
  const [text, setText] = useState(() => value.toFixed(places));
  useEffect(() => {
    setText(value.toFixed(places));
  }, [value, places]);

  const commit = (raw: string) => {
    const n = Number(raw);
    if (!Number.isFinite(n)) {
      setText(value.toFixed(places));
      return;
    }
    const clamped = Math.min(max, Math.max(min, n));
    onChange(clamped);
    setText(clamped.toFixed(places));
  };

  return (
    <label className="param-num" title={title}>
      <span className="param-num-label">{label}</span>
      <div className="param-num-row">
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={value}
          onChange={(e) => onChange(Number(e.target.value))}
          onDoubleClick={() => onReset?.()}
        />
        <input
          className="param-num-input"
          type="number"
          min={min}
          max={max}
          step={step}
          value={text}
          onChange={(e) => setText(e.target.value)}
          onBlur={() => commit(text)}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              e.currentTarget.blur();
            }
          }}
        />
        {suffix && <span className="param-num-suffix">{suffix}</span>}
      </div>
    </label>
  );
}

function StereoVu({
  title,
  peakL,
  peakR,
  clipping,
}: {
  title: string;
  peakL: number;
  peakR: number;
  clipping?: boolean;
}) {
  const dbL = peakToDb(peakL);
  const dbR = peakToDb(peakR);
  const pctL = Math.min(100, ((dbL + 60) / 60) * 100);
  const pctR = Math.min(100, ((dbR + 60) / 60) * 100);
  return (
    <div className={`stereo-vu ${clipping ? "clipping" : ""}`} title={clipping ? `${title} CLIP` : title}>
      <span className="stereo-vu-title">{title}{clipping ? " !" : ""}</span>
      <div className="stereo-vu-rows">
        <div className="stereo-vu-row">
          <span>L</span>
          <div className="meter-track">
            <div className={`meter-fill ${pctL > 95 ? "hot" : ""}`} style={{ width: `${pctL}%` }} />
          </div>
        </div>
        <div className="stereo-vu-row">
          <span>R</span>
          <div className="meter-track">
            <div className={`meter-fill ${pctR > 95 ? "hot" : ""}`} style={{ width: `${pctR}%` }} />
          </div>
        </div>
      </div>
    </div>
  );
}

export default function App() {
  const [graph, setGraph] = useState<GraphDocument | null>(null);
  const [library, setLibrary] = useState<LibraryItem[]>([]);
  const [libFilter, setLibFilter] = useState<LibFilter>("all");
  const [libQuery, setLibQuery] = useState("");
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [cpu, setCpu] = useState(0);
  const [masterInDb, setMasterInDb] = useState(0);
  const [masterOutDb, setMasterOutDb] = useState(-6);
  const [clipping, setClipping] = useState(false);
  const [io, setIo] = useState<IoInfo>({
    numInputs: 2,
    numOutputs: 2,
    inputPeak: 0,
    outputPeak: 0,
    inputPeakL: 0,
    inputPeakR: 0,
    outputPeakL: 0,
    outputPeakR: 0,
    activeInputChannel: 0,
    activeOutputChannel: 0,
    isStandalone: false,
  });
  const [dspStatus, setDspStatus] = useState<
    Record<
      string,
      {
        loaded: boolean;
        error?: string;
        filePath?: string;
        mediaPositionSec?: number;
        mediaDurationSec?: number;
        mediaPlaying?: boolean;
      }
    >
  >({});
  const [ytQuery, setYtQuery] = useState("");
  const [ytHits, setYtHits] = useState<
    { id: string; title: string; channel?: string; duration?: number; url?: string }[]
  >([]);
  const [ytBusy, setYtBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [toneOpen, setToneOpen] = useState(false);
  const [presetsOpen, setPresetsOpen] = useState(false);
  const [presets, setPresets] = useState<string[]>([]);
  const [presetName, setPresetName] = useState("");
  const [toneQuery, setToneQuery] = useState("");
  const [toneFormat, setToneFormat] = useState<"nam" | "ir" | "">("nam");
  const [toneSort, setToneSort] = useState("trending");
  const [toneGear, setToneGear] = useState("");
  const [toneArch, setToneArch] = useState("2");
  const [toneSize, setToneSize] = useState("");
  const [toneCalibrated, setToneCalibrated] = useState(false);
  const [toneBrowse, setToneBrowse] = useState<"browse" | "favorites" | "created" | "downloaded">("browse");
  const [tonePage, setTonePage] = useState(1);
  const [toneFiltersOpen, setToneFiltersOpen] = useState(() => {
    try {
      return localStorage.getItem("namplifier.toneFiltersOpen") !== "0";
    } catch {
      return true;
    }
  });
  const [tones, setTones] = useState<ToneHit[]>([]);
  const [favoritedIds, setFavoritedIds] = useState<Set<string>>(() => new Set());
  const [toneModels, setToneModels] = useState<{ id: string; name: string; architecture?: string; size?: string }[]>(
    []
  );
  const [signedIn, setSignedIn] = useState(false);
  const [loginWaiting, setLoginWaiting] = useState(false);
  const [busy, setBusy] = useState<string | null>(null);
  const [dragOverCanvas, setDragOverCanvas] = useState(false);
  const [view, setView] = useState({ x: 0, y: 0 });
  const [zoom, setZoom] = useState(1);
  const [panning, setPanning] = useState(false);
  const [toneW, setToneW] = useState(() => {
    try {
      const v = Number(localStorage.getItem("namplifier.toneW"));
      if (Number.isFinite(v) && v >= 280 && v <= 900) return v;
    } catch {
      /* ignore */
    }
    return 380;
  });
  const toneDragRef = useRef<{ startX: number; startW: number } | null>(null);
  const [inspectorH, setInspectorH] = useState(() => {
    try {
      const v = Number(localStorage.getItem("namplifier.inspectorH"));
      if (Number.isFinite(v) && v >= 120 && v <= 480) return v;
    } catch {
      /* ignore */
    }
    return 180;
  });
  const inspectorDragRef = useRef<{ startY: number; startH: number } | null>(null);
  const viewRef = useRef(view);
  viewRef.current = view;
  const zoomRef = useRef(zoom);
  zoomRef.current = zoom;
  const dragRef = useRef<{ id: string; ox: number; oy: number } | null>(null);
  const panRef = useRef<{ ox: number; oy: number; vx: number; vy: number } | null>(null);
  const connectRef = useRef<{
    nodeId: string;
    port: number;
    dir: "in" | "out";
    x: number;
    y: number;
  } | null>(null);
  const [wire, setWire] = useState<{ x1: number; y1: number; x2: number; y2: number } | null>(null);
  const canvasRef = useRef<HTMLDivElement>(null);
  const centerRef = useRef<HTMLDivElement>(null);
  const graphRef = useRef(graph);
  const selectedIdRef = useRef(selectedId);
  graphRef.current = graph;
  selectedIdRef.current = selectedId;

  const selected = useMemo(
    () => graph?.nodes.find((n) => n.id === selectedId) ?? null,
    [graph, selectedId]
  );

  const worldSize = useMemo(() => {
    let maxX = 4000;
    let maxY = 3000;
    for (const n of graph?.nodes ?? []) {
      maxX = Math.max(maxX, n.x + NODE_W + 400);
      maxY = Math.max(maxY, n.y + 280);
    }
    if (wire) {
      maxX = Math.max(maxX, wire.x1 + 200, wire.x2 + 200);
      maxY = Math.max(maxY, wire.y1 + 200, wire.y2 + 200);
    }
    return {
      w: Math.ceil(Math.min(24000, Math.max(8000, maxX))),
      h: Math.ceil(Math.min(18000, Math.max(6000, maxY))),
    };
  }, [graph?.nodes, wire]);

  const filteredLibrary = useMemo(() => {
    const q = libQuery.trim().toLowerCase();
    const standalone = io.isStandalone === true;
    return library.filter((item) => {
      // Host Output remaps interface outs — only meaningful in standalone.
      if (!standalone && (item.format === "host_output" || item.name === "Host Output"))
        return false;
      if (libFilter !== "all" && item.kind !== libFilter) return false;
      if (!q) return true;
      return item.name.toLowerCase().includes(q) || (item.source ?? "").toLowerCase().includes(q);
    });
  }, [library, libFilter, libQuery, io.isStandalone]);

  const refreshLibrary = useCallback(async () => {
    const res = await native.getLibrary();
    if (res.ok) setLibrary((res.data as LibraryItem[]) ?? []);
  }, []);

  const applyGraph = useCallback((raw: GraphDocument) => {
    const normalized = normalizeGraphPorts(raw);
    graphRef.current = normalized;
    setGraph(normalized);
    return normalized;
  }, []);

  const clientToWorld = useCallback((clientX: number, clientY: number) => {
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    const v = viewRef.current;
    const z = zoomRef.current;
    return { x: (clientX - rect.left - v.x) / z, y: (clientY - rect.top - v.y) / z };
  }, []);

  const refresh = useCallback(async () => {
    const res = await native.getGraph();
    if (res.ok) applyGraph(res.data as GraphDocument);
    else setError(res.error ?? "Failed to load graph");
    const st = await native.toneStatus();
    if (st.ok) setSignedIn(Boolean((st.data as { signedIn?: boolean })?.signedIn));
    const pr = await native.listPresets();
    if (pr.ok) setPresets((pr.data as string[]) ?? []);
    const m = await native.getMaster();
    if (m.ok) {
      const d = m.data as { inputDb?: number; outputDb?: number };
      if (typeof d.inputDb === "number") setMasterInDb(d.inputDb);
      if (typeof d.outputDb === "number") setMasterOutDb(d.outputDb);
    }
    await refreshLibrary();
  }, [applyGraph, refreshLibrary]);

  useEffect(() => {
    void refresh();
    onNativeEvent("graphChanged", () => void refresh());
    onNativeEvent("libraryChanged", () => void refreshLibrary());
    onNativeEvent("toneAuthFinished", () => {
      setLoginWaiting(false);
      void refresh();
    });
    const t = window.setInterval(async () => {
      const c = await native.getCpu();
      if (c.ok) setCpu(Number(c.data) || 0);
      const info = await native.getIoInfo();
      if (info.ok) {
        const data = info.data as IoInfo;
        setIo(data);
        setClipping(Boolean(data.clipping));
      }
      const st = await native.getDspStatus();
      if (st.ok) {
        const map: Record<
          string,
          {
            loaded: boolean;
            error?: string;
            filePath?: string;
            mediaPositionSec?: number;
            mediaDurationSec?: number;
            mediaPlaying?: boolean;
          }
        > = {};
        for (const row of (st.data as {
          id: string;
          loaded: boolean;
          error?: string;
          filePath?: string;
          mediaPositionSec?: number;
          mediaDurationSec?: number;
          mediaPlaying?: boolean;
        }[]) ?? []) {
          map[row.id] = {
            loaded: row.loaded,
            error: row.error,
            filePath: row.filePath,
            mediaPositionSec: row.mediaPositionSec,
            mediaDurationSec: row.mediaDurationSec,
            mediaPlaying: row.mediaPlaying,
          };
        }
        setDspStatus(map);

        // Keep graph mediaPlaying in sync when transport stops naturally.
        const g = graphRef.current;
        if (g) {
          let changed = false;
          const nodes = g.nodes.map((n) => {
            if (n.type !== "media" && n.type !== "youtube") return n;
            const playing = Boolean(map[n.id]?.mediaPlaying);
            if (Boolean(n.params.mediaPlaying) !== playing) {
              changed = true;
              return { ...n, params: { ...n.params, mediaPlaying: playing } };
            }
            return n;
          });
          if (changed) {
            const next = { ...g, nodes };
            graphRef.current = next;
            setGraph(next);
          }
        }
      }
    }, 250);
    return () => window.clearInterval(t);
  }, [refresh, refreshLibrary]);

  useEffect(() => {
    const move = (ev: PointerEvent) => {
      if (panRef.current) {
        const { ox, oy, vx, vy } = panRef.current;
        setView({ x: vx + (ev.clientX - ox), y: vy + (ev.clientY - oy) });
        return;
      }
      if (connectRef.current) {
        const w = clientToWorld(ev.clientX, ev.clientY);
        setWire({
          x1: connectRef.current.x,
          y1: connectRef.current.y,
          x2: w.x,
          y2: w.y,
        });
        return;
      }
      if (!dragRef.current || !graphRef.current) return;
      const { id, ox, oy } = dragRef.current;
      const w = clientToWorld(ev.clientX, ev.clientY);
      const g = graphRef.current;
      const next = {
        ...g,
        nodes: g.nodes.map((n) => (n.id === id ? { ...n, x: w.x - ox, y: w.y - oy } : n)),
      };
      graphRef.current = next;
      setGraph(next);
    };

    const end = (ev: PointerEvent) => {
      if (panRef.current) {
        panRef.current = null;
        setPanning(false);
        return;
      }

      if (connectRef.current) {
        const drag = connectRef.current;
        connectRef.current = null;
        setWire(null);
        const el = document.elementFromPoint(ev.clientX, ev.clientY);
        const g = graphRef.current;
        if (!g) return;

        const wantDir = drag.dir === "out" ? "in" : "out";
        const portEl = el?.closest?.(`[data-port='${wantDir}']`) as HTMLElement | null;
        const otherId = portEl?.getAttribute("data-node-id");
        const otherPort = Number(portEl?.getAttribute("data-port-index") ?? "0");
        if (!otherId || otherId === drag.nodeId) return;

        const fromNode = drag.dir === "out" ? drag.nodeId : otherId;
        const fromPort = drag.dir === "out" ? drag.port : otherPort;
        const toNode = drag.dir === "out" ? otherId : drag.nodeId;
        const toPort = drag.dir === "out" ? otherPort : drag.port;

        const fromN = g.nodes.find((n) => n.id === fromNode);
        const toN = g.nodes.find((n) => n.id === toNode);
        if (!fromN || !toN) return;
        if (fromPort < 0 || fromPort >= numOutPorts(fromN)) return;
        if (toPort < 0 || toPort >= numInPorts(toN)) return;

        const edges = g.edges.filter(
          (e) =>
            !(e.fromNode === fromNode && e.fromPort === fromPort) &&
            !(e.toNode === toNode && e.toPort === toPort)
        );
        edges.push({
          id: uid("e"),
          fromNode,
          fromPort,
          toNode,
          toPort,
        });
        const next = applyGraph({ ...g, edges });
        void native.setGraph(next);
        return;
      }

      const wasDragging = dragRef.current;
      const g = graphRef.current;
      dragRef.current = null;
      if (wasDragging && g) void native.setGraph(g);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", end);
    window.addEventListener("pointercancel", end);
    const onBlur = () => {
      connectRef.current = null;
      dragRef.current = null;
      panRef.current = null;
      setWire(null);
      setPanning(false);
    };
    window.addEventListener("blur", onBlur);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", end);
      window.removeEventListener("pointercancel", end);
      window.removeEventListener("blur", onBlur);
    };
  }, [applyGraph, clientToWorld]);

  const pushGraph = async (next: GraphDocument) => {
    const normalized = applyGraph(next);
    const res = await native.setGraph(normalized);
    if (!res.ok) setError(res.error ?? "setGraph failed");
  };

  const deleteSelected = useCallback(async () => {
    const g = graphRef.current;
    const sid = selectedIdRef.current;
    if (!g || !sid) return;
    const n = g.nodes.find((x) => x.id === sid);
    if (!n) return;
    if (n.type === "input" && g.nodes.filter((x) => x.type === "input").length <= 1) {
      setError("Keep at least one Input — add another from Library → Routing first.");
      return;
    }
    if (n.type === "output" && g.nodes.filter((x) => x.type === "output").length <= 1) {
      setError("Keep at least one Output — add another from Library → Routing first.");
      return;
    }
    setError(null);
    await pushGraph({
      ...g,
      nodes: g.nodes.filter((x) => x.id !== sid),
      edges: g.edges.filter((e) => e.fromNode !== sid && e.toNode !== sid),
    });
    setSelectedId(null);
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;
      if (e.key === "Delete" || e.key === "Backspace") {
        e.preventDefault();
        void deleteSelected();
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [deleteSelected]);

  useEffect(() => {
    const el = centerRef.current;
    if (!el) return;
    const clampInspector = () => {
      const max = Math.max(120, Math.floor(el.clientHeight * 0.45));
      setInspectorH((h) => (h > max ? max : h));
    };
    const ro = new ResizeObserver(clampInspector);
    ro.observe(el);
    clampInspector();
    return () => ro.disconnect();
  }, [graph]);

  useEffect(() => {
    const el = canvasRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      // Plain wheel zooms; Ctrl/Cmd also zooms (trackpad pinch often sends ctrlKey).
      e.preventDefault();
      const z0 = zoomRef.current;
      const z1 = Math.min(2.5, Math.max(0.4, z0 * (e.deltaY < 0 ? 1.1 : 1 / 1.1)));
      if (Math.abs(z1 - z0) < 1e-4) return;
      const rect = el.getBoundingClientRect();
      const v = viewRef.current;
      const wx = (e.clientX - rect.left - v.x) / z0;
      const wy = (e.clientY - rect.top - v.y) / z0;
      setView({
        x: e.clientX - rect.left - wx * z1,
        y: e.clientY - rect.top - wy * z1,
      });
      setZoom(z1);
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [graph]);

  useEffect(() => {
    const onMove = (ev: PointerEvent) => {
      if (!inspectorDragRef.current) return;
      const dy = inspectorDragRef.current.startY - ev.clientY; // drag up = taller
      const centerH = centerRef.current?.clientHeight ?? window.innerHeight;
      const maxH = Math.min(480, Math.floor(centerH * 0.45));
      const next = Math.max(120, Math.min(maxH, inspectorDragRef.current.startH + dy));
      setInspectorH(next);
    };
    const onUp = () => {
      if (!inspectorDragRef.current) return;
      inspectorDragRef.current = null;
      setInspectorH((h) => {
        try {
          localStorage.setItem("namplifier.inspectorH", String(h));
        } catch {
          /* ignore */
        }
        return h;
      });
    };
    window.addEventListener("pointermove", onMove);
    window.addEventListener("pointerup", onUp);
    window.addEventListener("pointercancel", onUp);
    return () => {
      window.removeEventListener("pointermove", onMove);
      window.removeEventListener("pointerup", onUp);
      window.removeEventListener("pointercancel", onUp);
    };
  }, []);

  const startInspectorResize = (e: React.PointerEvent) => {
    e.preventDefault();
    e.stopPropagation();
    inspectorDragRef.current = { startY: e.clientY, startH: inspectorH };
    try {
      (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    } catch {
      /* ignore */
    }
  };

  useEffect(() => {
    if (!selected?.params.toneId || (selected.type !== "nam" && selected.type !== "ir")) {
      setToneModels([]);
      return;
    }
    const fmt = selected.type === "ir" ? "ir" : "nam";
    let cancelled = false;
    void native.toneGetModels(selected.params.toneId, fmt).then((res) => {
      if (cancelled) return;
      if (res.ok) setToneModels((res.data as { id: string; name: string; architecture?: string; size?: string }[]) ?? []);
      else setToneModels([]);
    });
    return () => {
      cancelled = true;
    };
  }, [selected?.id, selected?.params.toneId, selected?.type]);

  const addNode = async (type: NodeType) => {
    if (!graph) return;
    if (type === "input" || type === "output" || type === "fx") return;
    const node: GraphNode = {
      id: uid(type),
      type,
      x: 180 + Math.random() * 220,
      y: 140 + Math.random() * 180,
      params: { ...emptyParams(), displayName: type.toUpperCase() },
    };
    await pushGraph({ ...graph, nodes: [...graph.nodes, node] });
    setSelectedId(node.id);
  };

  const updateParams = async (params: NodeParams) => {
    if (!graph || !selectedId) return;
    const next = {
      ...graph,
      nodes: graph.nodes.map((n) => (n.id === selectedId ? { ...n, params } : n)),
    };
    graphRef.current = next;
    setGraph(next);
    await native.updateNode(selectedId, params);
  };

  const removeEdge = async (edgeId: string) => {
    if (!graph) return;
    await pushGraph({ ...graph, edges: graph.edges.filter((e) => e.id !== edgeId) });
  };

  const ZOOM_MIN = 0.4;
  const ZOOM_MAX = 2.5;

  const applyZoom = (nextZoom: number, anchorClientX?: number, anchorClientY?: number) => {
    const z0 = zoomRef.current;
    const z1 = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, nextZoom));
    if (Math.abs(z1 - z0) < 1e-4) return;
    if (anchorClientX != null && anchorClientY != null && canvasRef.current) {
      const rect = canvasRef.current.getBoundingClientRect();
      const v = viewRef.current;
      const wx = (anchorClientX - rect.left - v.x) / z0;
      const wy = (anchorClientY - rect.top - v.y) / z0;
      setView({
        x: anchorClientX - rect.left - wx * z1,
        y: anchorClientY - rect.top - wy * z1,
      });
    }
    setZoom(z1);
  };

  const onPointerDownNode = (e: React.PointerEvent, id: string) => {
    if (e.button !== 0) return;
    const target = e.target as HTMLElement | null;
    if (target?.closest?.(".port")) return;
    e.stopPropagation();
    setSelectedId(id);
    const n = graph?.nodes.find((x) => x.id === id);
    if (!n) return;
    const w = clientToWorld(e.clientX, e.clientY);
    dragRef.current = { id, ox: w.x - n.x, oy: w.y - n.y };
    connectRef.current = null;
    setWire(null);
    try {
      (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    } catch {
      /* ignore */
    }
  };

  const startConnect = (
    e: React.PointerEvent,
    nodeId: string,
    dir: "in" | "out",
    portIndex: number
  ) => {
    if (e.button !== 0) return;
    e.stopPropagation();
    e.preventDefault();
    dragRef.current = null;
    panRef.current = null;
    const n = graph?.nodes.find((x) => x.id === nodeId);
    if (!n) return;
    const p = portPos(n, dir, portIndex);
    connectRef.current = { nodeId, port: portIndex, dir, x: p.x, y: p.y };
    setWire({ x1: p.x, y1: p.y, x2: p.x, y2: p.y });
  };

  const onCanvasPointerDown = (e: React.PointerEvent) => {
    if (e.button === 2 || e.button === 1) {
      e.preventDefault();
      setPanning(true);
      panRef.current = {
        ox: e.clientX,
        oy: e.clientY,
        vx: viewRef.current.x,
        vy: viewRef.current.y,
      };
      dragRef.current = null;
      connectRef.current = null;
      setWire(null);
      try {
        (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
      } catch {
        /* ignore */
      }
      return;
    }
    if (e.button === 0) {
      // Cancel an unfinished cable if clicking empty canvas
      if (connectRef.current) {
        connectRef.current = null;
        setWire(null);
      }
      setSelectedId(null);
    }
  };

  const dropLibraryOnCanvas = async (libraryId: string, clientX: number, clientY: number) => {
    const w = clientToWorld(clientX, clientY);
    const x = w.x - NODE_W / 2;
    const y = w.y - 36;
    setError(null);
    const res = await native.libraryAddToGraph(libraryId, x, y);
    if (!res.ok) setError(res.error ?? "drop failed");
    else {
      applyGraph(res.data as GraphDocument);
      const nodes = (res.data as GraphDocument).nodes;
      // Prefer the node closest to the drop point (last is usually correct)
      const dropped =
        nodes.find((n) => Math.abs(n.x - x) < 2 && Math.abs(n.y - y) < 2) ??
        nodes[nodes.length - 1];
      setSelectedId(dropped?.id ?? null);
    }
  };

  const dropToneOnCanvas = async (t: ToneHit, clientX: number, clientY: number) => {
    const w = clientToWorld(clientX, clientY);
    const x = w.x - NODE_W / 2;
    const y = w.y - 36;
    setError(null);
    setBusy(`Adding ${t.name}…`);
    const res = await native.toneAddToGraph(
      t.id,
      t.name,
      t.format || toneFormat,
      t.imageUrl,
      t.cabIncluded,
      x,
      y
    );
    setBusy(null);
    if (!res.ok) setError(res.error ?? "drop failed");
    else {
      const data = res.data as { graph: GraphDocument; library: LibraryItem[] };
      applyGraph(data.graph);
      setLibrary(data.library ?? []);
      const nodes = data.graph.nodes;
      const dropped =
        nodes.find((n) => Math.abs(n.x - x) < 2 && Math.abs(n.y - y) < 2) ??
        nodes[nodes.length - 1];
      setSelectedId(dropped?.id ?? null);
    }
  };

  /** Center of the visible canvas in world coords — used when clicking Add (not drag). */
  const canvasDropAnchor = () => {
    const el = canvasRef.current;
    if (!el) return { x: 240, y: 180 };
    const r = el.getBoundingClientRect();
    const w = clientToWorld(r.left + r.width * 0.5, r.top + r.height * 0.45);
    return { x: w.x - NODE_W / 2, y: w.y - 36 };
  };

  const TONE_PAGE_SIZE = 20;

  const applyToneResults = (list: ToneHit[]) => {
    setTones(list);
    void refreshFavoritedIds();
  };

  const searchTones = async (page = 1) => {
    setError(null);
    setBusy(page > 1 ? `Page ${page}…` : "Searching…");
    setToneBrowse("browse");
    setTonePage(page);
    const sort =
      toneSort || (toneQuery.trim() ? "best-match" : "trending");
    const res = await native.toneSearch({
      query: toneQuery,
      format: toneFormat,
      architecture: toneFormat === "ir" ? "" : toneArch,
      sort,
      gears: toneFormat === "ir" ? "" : toneGear,
      sizes: toneFormat === "ir" ? "" : toneSize,
      calibrated: toneCalibrated,
      page,
      pageSize: TONE_PAGE_SIZE,
    });
    setBusy(null);
    if (!res.ok) {
      setError(res.error ?? "search failed");
      return;
    }
    applyToneResults((res.data as ToneHit[]) ?? []);
    setToneFiltersOpen(false);
    try {
      localStorage.setItem("namplifier.toneFiltersOpen", "0");
    } catch {
      /* ignore */
    }
  };

  const loadToneCollection = async (
    mode: "favorites" | "created" | "downloaded",
    page = 1
  ) => {
    setError(null);
    setBusy(mode === "favorites" ? "Loading favorites…" : mode === "created" ? "Loading yours…" : "Loading downloads…");
    setToneBrowse(mode);
    setTonePage(page);
    const res =
      mode === "favorites"
        ? await native.toneFavorites(page, TONE_PAGE_SIZE)
        : mode === "created"
          ? await native.toneCreated(page, TONE_PAGE_SIZE)
          : await native.toneDownloaded(page, TONE_PAGE_SIZE);
    setBusy(null);
    if (!res.ok) {
      setError(res.error ?? `${mode} failed`);
      return;
    }
    const list = (res.data as ToneHit[]) ?? [];
    setTones(list);
    if (mode === "favorites") setFavoritedIds(new Set(list.map((t) => t.id)));
    else void refreshFavoritedIds();
  };

  const refreshFavoritedIds = async () => {
    const res = await native.toneFavorites(1, 50);
    if (res.ok) {
      const list = (res.data as ToneHit[]) ?? [];
      setFavoritedIds(new Set(list.map((t) => t.id)));
    }
  };

  const reloadTonePage = async (page = tonePage) => {
    if (toneBrowse === "browse") await searchTones(page);
    else await loadToneCollection(toneBrowse, page);
  };

  const toggleFavorite = async (t: ToneHit) => {
    if (!signedIn) {
      setError("Sign in to star tones on Tone3000.");
      return;
    }
    const next = !favoritedIds.has(t.id);
    const res = await native.toneFavorite(t.id, next);
    if (!res.ok) {
      setError(res.error ?? "Could not update favorite");
      return;
    }
    setFavoritedIds((prev) => {
      const s = new Set(prev);
      if (next) s.add(t.id);
      else s.delete(t.id);
      return s;
    });
    setTones((prev) =>
      prev.map((row) =>
        row.id === t.id
          ? { ...row, favorited: next, favorites: Math.max(0, (row.favorites ?? 0) + (next ? 1 : -1)) }
          : row
      )
    );
  };

  const addToneToLibrary = async (t: ToneHit) => {
    setBusy(`Adding ${t.name}…`);
    setError(null);
    const res = await native.toneAddToLibrary(
      t.id,
      t.name,
      t.format || toneFormat || "nam",
      t.imageUrl,
      t.cabIncluded
    );
    setBusy(null);
    if (!res.ok) setError(res.error ?? "add failed");
    else setLibrary((res.data as LibraryItem[]) ?? []);
  };

  const useToneItem = async (t: ToneHit) => {
    setError(null);
    const fmt = (t.format || toneFormat || "nam").toLowerCase();
    const isIr = fmt === "ir";
    if (selected && ((selected.type === "nam" && !isIr) || (selected.type === "ir" && isIr))) {
      setBusy(`Loading ${t.name}…`);
      const res = await native.toneLoadToNode(
        selected.id,
        t.id,
        fmt,
        undefined,
        t.name,
        t.imageUrl,
        t.cabIncluded
      );
      setBusy(null);
      if (!res.ok) setError(res.error ?? "load failed");
      else {
        const data = res.data as { graph: GraphDocument; library: LibraryItem[] };
        applyGraph(data.graph);
        if (data.library) setLibrary(data.library);
      }
      return;
    }
    setBusy(`Adding ${t.name}…`);
    const a = canvasDropAnchor();
    const res = await native.toneAddToGraph(t.id, t.name, fmt, t.imageUrl, t.cabIncluded, a.x, a.y);
    setBusy(null);
    if (!res.ok) setError(res.error ?? "add failed");
    else {
      const data = res.data as { graph: GraphDocument; library: LibraryItem[] };
      applyGraph(data.graph);
      setLibrary(data.library ?? []);
      const nodes = data.graph.nodes;
      setSelectedId(nodes[nodes.length - 1]?.id ?? null);
    }
  };

  const useLibraryItem = async (item: LibraryItem) => {
    setError(null);
    if (item.kind === "fx" && !isReadyFx(item)) {
      setError("That FX isn’t available.");
      return;
    }
    const anchor = canvasDropAnchor();
    if (item.kind === "fx" || isRoutingItem(item)) {
      const res = await native.libraryAddToGraph(item.id, anchor.x, anchor.y);
      if (!res.ok) setError(res.error ?? "add failed");
      else {
        applyGraph(res.data as GraphDocument);
        const nodes = (res.data as GraphDocument).nodes;
        setSelectedId(nodes[nodes.length - 1]?.id ?? null);
      }
      return;
    }
    if (selected && (selected.type === "nam" || selected.type === "ir")) {
      if (item.kind === "nam" && selected.type === "ir") {
        setError("Amps can’t load into IR nodes — drop on the graph or a NAM block.");
        return;
      }
      if (item.kind === "ir" && selected.type === "nam") {
        setError("IRs can’t load into NAM nodes — drop on the graph or an IR block.");
        return;
      }
      const res = await native.libraryApplyToNode(selected.id, item.id);
      if (!res.ok) setError(res.error ?? "apply failed");
      else applyGraph(res.data as GraphDocument);
      return;
    }
    const res = await native.libraryAddToGraph(item.id, anchor.x, anchor.y);
    if (!res.ok) setError(res.error ?? "add to graph failed");
    else {
      applyGraph(res.data as GraphDocument);
      const nodes = (res.data as GraphDocument).nodes;
      setSelectedId(nodes[nodes.length - 1]?.id ?? null);
    }
  };

  if (!graph) {
    return (
      <div className="app">
        <div className="topbar">
          <div className="brand">
            Namplifier<span>.</span>
          </div>
        </div>
        <div className="hint" style={{ padding: 24 }}>
          Loading…
        </div>
      </div>
    );
  }

  const p = selected?.params ?? emptyParams();
  const inputChoices = Math.max(1, io.numInputs || 2);

  return (
    <div
      className={`app ${toneOpen ? "tone-open" : ""}`}
      style={toneOpen ? ({ ["--tone-w" as string]: `${toneW}px` } as Record<string, string>) : undefined}
    >
      <header className="topbar">
        <div className="brand">
          Namplifier<span>.</span>
        </div>
        <button className="pill" onClick={() => void deleteSelected()}>
          Delete
        </button>
        <div className="spacer" />
        <div className="master-strip">
          <label className="master-fader">
            <span>In</span>
            <input
              type="range"
              min={-24}
              max={12}
              step={0.5}
              value={masterInDb}
              onChange={(e) => {
                const v = Number(e.target.value);
                setMasterInDb(v);
                void native.setMaster(v, undefined);
              }}
              onDoubleClick={() => {
                setMasterInDb(0);
                void native.setMaster(0, undefined);
              }}
            />
            <em>{masterInDb.toFixed(1)} dB</em>
          </label>
          <label className="master-fader">
            <span>Out</span>
            <input
              type="range"
              min={-24}
              max={6}
              step={0.5}
              value={masterOutDb}
              onChange={(e) => {
                const v = Number(e.target.value);
                setMasterOutDb(v);
                void native.setMaster(undefined, v);
              }}
              onDoubleClick={() => {
                setMasterOutDb(-6);
                void native.setMaster(undefined, -6);
              }}
            />
            <em>{masterOutDb.toFixed(1)} dB</em>
          </label>
        </div>
        <button className={`pill ${presetsOpen ? "active" : ""}`} onClick={() => setPresetsOpen((v) => !v)}>
          Presets
        </button>
        <button
          className={`pill primary ${toneOpen ? "active" : ""}`}
          onClick={() => {
            setToneOpen((v) => {
              const next = !v;
              if (next) window.setTimeout(() => void searchTones(1), 0);
              return next;
            });
          }}
        >
          Tone3000
        </button>
        <div className="cpu">
          <span className="cpu-pct">{(cpu * 100).toFixed(0)}</span>% CPU
          {io.buildId ? <span className="muted"> · {io.buildId}</span> : null}
        </div>
        <div
          className={`auth-dot ${signedIn ? "on" : "off"}`}
          title={signedIn ? "Tone3000 signed in" : "Tone3000 signed out"}
          aria-label={signedIn ? "Tone3000 signed in" : "Tone3000 signed out"}
        />
        <div className="vu-strip">
          <StereoVu title="IN" peakL={io.inputPeakL ?? 0} peakR={io.inputPeakR ?? 0} />
          <StereoVu
            title="OUT"
            peakL={io.outputPeakL ?? 0}
            peakR={io.outputPeakR ?? 0}
            clipping={clipping}
          />
        </div>
      </header>

      {io.inputMuted && (
        <div className="audio-banner">
          Audio input may still be muted in Options → Audio Settings (uncheck &quot;Mute audio input&quot;).
          That&apos;s why guitar is silent while Test still plays a tone — and why Test can squeal if
          speakers/monitor feed your guitar.
          <button
            className="pill tiny primary"
            onClick={async () => {
              await native.unmuteInput();
              setError("Saved unmute preference — also uncheck Mute in Settings, or restart the app.");
            }}
          >
            Fix settings file
          </button>
        </div>
      )}

      <div className="layout">
        <aside className="library">
          <div className="panel-head">
            <h2>Library</h2>
            <div className="row tight">
              <button className="pill tiny" onClick={() => void native.libraryAddLocal("nam")}>
                + NAM
              </button>
              <button className="pill tiny" onClick={() => void native.libraryAddLocal("ir")}>
                + IR
              </button>
            </div>
          </div>
          <input
            className="search"
            value={libQuery}
            onChange={(e) => setLibQuery(e.target.value)}
            placeholder="Filter library…"
          />
          <div className="chips">
            {(["all", "nam", "ir", "fx", "routing"] as LibFilter[]).map((f) => (
              <button key={f} className={libFilter === f ? "chip active" : "chip"} onClick={() => setLibFilter(f)}>
                {f}
              </button>
            ))}
          </div>
          <div className="lib-scroll">
            {filteredLibrary.map((item) => (
              <div
                key={item.id}
                className={`lib-item kind-${item.kind}`}
                draggable={isReadyFx(item) || isRoutingItem(item) || item.kind !== "fx"}
                onDragStart={(e) => {
                  if (item.kind === "fx" && !isReadyFx(item)) {
                    e.preventDefault();
                    return;
                  }
                  e.dataTransfer.setData("application/x-namplifier-lib", item.id);
                  e.dataTransfer.effectAllowed = "copy";
                }}
              >
                <button
                  className="lib-main"
                  onClick={() => void useLibraryItem(item)}
                >
                  {item.imageUrl ? (
                    <img className="lib-thumb" src={item.imageUrl} alt="" loading="lazy" />
                  ) : (
                    <div className={`lib-thumb placeholder kind-${item.kind}`}>{item.kind}</div>
                  )}
                  <div className="lib-copy">
                    <span className="kind-tag">{item.kind}</span>
                    {item.cabIncluded && <span className="cab-tag">cab in</span>}
                    <span className="lib-name">{item.name}</span>
                    <small>
                      {item.source || "local"}
                      {item.cabIncluded ? " · cab included" : ""}
                      {item.notes && !item.notes.toLowerCase().includes("cab") ? ` · ${item.notes}` : ""}
                    </small>
                  </div>
                </button>
                {item.source !== "factory" && (
                  <button
                    className="pill tiny danger"
                    onClick={() => void native.libraryRemove(item.id).then(refreshLibrary)}
                  >
                    ×
                  </button>
                )}
              </div>
            ))}
            {filteredLibrary.length === 0 && <p className="hint">No items</p>}
          </div>
        </aside>

        <div
          className="center"
          ref={centerRef}
          style={{ ["--inspector-h" as string]: `${inspectorH}px` }}
        >
          <div
            className={`canvas ${dragOverCanvas ? "drop-target" : ""} ${panning ? "panning" : ""}`}
            ref={canvasRef}
            onPointerDown={onCanvasPointerDown}
            onContextMenu={(e) => e.preventDefault()}
            onDragOver={(e) => {
              if (
                e.dataTransfer.types.includes("application/x-namplifier-lib") ||
                e.dataTransfer.types.includes("application/x-namplifier-tone")
              ) {
                e.preventDefault();
                setDragOverCanvas(true);
              }
            }}
            onDragLeave={() => setDragOverCanvas(false)}
            onDrop={(e) => {
              e.preventDefault();
              setDragOverCanvas(false);
              const toneRaw = e.dataTransfer.getData("application/x-namplifier-tone");
              if (toneRaw) {
                try {
                  void dropToneOnCanvas(JSON.parse(toneRaw) as ToneHit, e.clientX, e.clientY);
                } catch {
                  setError("Bad tone drag data");
                }
                return;
              }
              const id = e.dataTransfer.getData("application/x-namplifier-lib");
              if (id) void dropLibraryOnCanvas(id, e.clientX, e.clientY);
            }}
            style={{
              backgroundPosition: `${view.x}px ${view.y}px`,
              backgroundSize: `${24 * zoom}px ${24 * zoom}px`,
            }}
          >
            <div
              className="canvas-world"
              style={{
                width: worldSize.w,
                height: worldSize.h,
                transform: `translate(${view.x}px, ${view.y}px) scale(${zoom})`,
              }}
            >
            <svg className="edge-layer interactive" width={worldSize.w} height={worldSize.h}>
              {graph.edges.map((e) => {
                const a = graph.nodes.find((n) => n.id === e.fromNode);
                const b = graph.nodes.find((n) => n.id === e.toNode);
                if (!a || !b) return null;
                const p1 = portPos(a, "out", e.fromPort ?? 0);
                const p2 = portPos(b, "in", e.toPort ?? 0);
                const mx = (p1.x + p2.x) / 2;
                const d = `M ${p1.x} ${p1.y} C ${mx} ${p1.y}, ${mx} ${p2.y}, ${p2.x} ${p2.y}`;
                return (
                  <g key={e.id}>
                    <path className="edge-hit" d={d} onClick={(ev) => {
                      if (ev.altKey) void removeEdge(e.id);
                    }} />
                    <path d={d} />
                  </g>
                );
              })}
              {wire && (
                <path
                  className="wire-draw"
                  d={`M ${wire.x1} ${wire.y1} C ${(wire.x1 + wire.x2) / 2} ${wire.y1}, ${
                    (wire.x1 + wire.x2) / 2
                  } ${wire.y2}, ${wire.x2} ${wire.y2}`}
                />
              )}
            </svg>
            {graph.nodes.map((n) => (
              <div
                key={n.id}
                className={`node ${n.type} ${selectedId === n.id ? "selected" : ""} ${
                  numInPorts(n) > 1 || numOutPorts(n) > 1 ? "multi-port" : ""
                }`}
                style={{ left: n.x, top: n.y }}
                onPointerDown={(e) => onPointerDownNode(e, n.id)}
              >
                {n.params.imageUrl && (n.type === "nam" || n.type === "ir") && (
                  <img className="node-thumb" src={n.params.imageUrl} alt="" draggable={false} />
                )}
                    <div className="title">
                  {isHostOutput(n) ? "host out" : n.type}
                  {n.params.cabIncluded ? " · cab in" : ""}
                  {isStereoFxNode(n) ? " · stereo" : ""}
                </div>
                <div className="name">{nodeLabel(n)}</div>
                {n.params.cabIncluded && <div className="node-cab">cab included — no IR needed</div>}
                {(n.type === "nam" || n.type === "ir" || n.type === "media" || n.type === "youtube") && (
                  <div className="node-warn">
                    {dspStatus[n.id]?.error
                      ? "load failed"
                      : dspStatus[n.id]?.loaded
                        ? ""
                        : n.params.filePath || dspStatus[n.id]?.filePath
                          ? "loading…"
                          : n.type === "media" || n.type === "youtube"
                            ? "no media"
                            : "no model"}
                  </div>
                )}
                {Array.from({ length: numInPorts(n) }, (_, i) => (
                  <div
                    key={`in-${i}`}
                    className={`port in ${numInPorts(n) > 1 ? "stacked" : ""}`}
                    data-port="in"
                    data-port-index={i}
                    data-node-id={n.id}
                    style={
                      numInPorts(n) > 1
                        ? { top: `${((i + 0.5) / numInPorts(n)) * 100}%` }
                        : undefined
                    }
                    title={portLabel(n, "in", i) || "In"}
                    onPointerDown={(e) => startConnect(e, n.id, "in", i)}
                  >
                    {portLabel(n, "in", i) && <span className="port-tag">{portLabel(n, "in", i)}</span>}
                  </div>
                ))}
                {Array.from({ length: numOutPorts(n) }, (_, i) => (
                  <div
                    key={`out-${i}`}
                    className={`port out ${numOutPorts(n) > 1 ? "stacked" : ""}`}
                    data-port="out"
                    data-port-index={i}
                    data-node-id={n.id}
                    style={
                      numOutPorts(n) > 1
                        ? { top: `${((i + 0.5) / numOutPorts(n)) * 100}%` }
                        : undefined
                    }
                    title={portLabel(n, "out", i) || "Out"}
                    onPointerDown={(e) => startConnect(e, n.id, "out", i)}
                  >
                    {portLabel(n, "out", i) && <span className="port-tag">{portLabel(n, "out", i)}</span>}
                  </div>
                ))}
              </div>
            ))}
            </div>
            <div className="zoom-controls">
              <button type="button" title="Zoom out" onClick={() => applyZoom(zoom / 1.15)}>
                −
              </button>
              <button type="button" className="zoom-pct" title="Reset zoom" onClick={() => applyZoom(1)}>
                {Math.round(zoom * 100)}%
              </button>
              <button type="button" title="Zoom in" onClick={() => applyZoom(zoom * 1.15)}>
                +
              </button>
            </div>
          </div>

          <div className="inspector-shell">
            <div
              className="inspector-resize"
              onPointerDown={startInspectorResize}
            />
            <div className="inspector-frame">
            {!selected && (
              <div className="inspector-empty">
                {error && <span className="error">{error}</span>}
                {busy && <span className="hint">{busy}</span>}
              </div>
            )}
            {selected && (
              <>
                <div className="inspector-head">
                  <div>
                    <strong>{nodeLabel(selected)}</strong>
                    <small className="muted"> · {selected.type}</small>
                  </div>
                  {error && <span className="error">{error}</span>}
                  {busy && <span className="hint">{busy}</span>}
                </div>

                {selected.type === "input" && (
                  <div className="inspector-grid">
                    <StereoVu title="IN" peakL={io.inputPeakL ?? 0} peakR={io.inputPeakR ?? 0} />
                    {io.isStandalone ? (
                      inputChoices > 1 && (
                        <label>
                          Interface input
                          <select
                            value={p.inputChannel ?? 0}
                            onChange={(e) =>
                              void updateParams({ ...p, inputChannel: Number(e.target.value) })
                            }
                          >
                            {Array.from({ length: inputChoices }, (_, i) => (
                              <option key={i} value={i}>
                                In {i + 1}
                              </option>
                            ))}
                          </select>
                          <small className="muted">Which device channel feeds this Input node.</small>
                        </label>
                      )
                    ) : (
                      <p className="hint">
                        Input comes from the DAW track — set hardware input / pins in the host, not here.
                      </p>
                    )}
                  </div>
                )}

                {(selected.type === "media" || selected.type === "youtube") && (
                  <div className="inspector-grid">
                    <div className="inspector-meta">
                      {dspStatus[selected.id]?.error && (
                        <p className="error">{dspStatus[selected.id].error}</p>
                      )}
                      {!dspStatus[selected.id]?.loaded &&
                        !!selected.params.filePath &&
                        !dspStatus[selected.id]?.error && (
                          <p className="hint">Loading audio…</p>
                        )}
                      {selected.type === "media" && (
                        <p className="hint">
                          {selected.params.filePath
                            ? selected.params.filePath.split(/[/\\]/).pop()
                            : "No file loaded"}
                        </p>
                      )}
                      {selected.type === "youtube" && selected.params.mediaUrl && (
                        <p className="hint">{selected.params.mediaUrl}</p>
                      )}
                    </div>

                    {selected.type === "media" && (
                      <button
                        className="pill tiny"
                        onClick={() => void native.pickFileForNode(selected.id, "media")}
                      >
                        Browse audio file
                      </button>
                    )}

                    {selected.type === "youtube" && (
                      <>
                        <label>
                          Search YouTube
                          <div className="media-search-row">
                            <input
                              value={ytQuery}
                              onChange={(e) => setYtQuery(e.target.value)}
                              onKeyDown={(e) => {
                                if (e.key === "Enter") {
                                  e.preventDefault();
                                  void (async () => {
                                    setYtBusy("Searching…");
                                    setError(null);
                                    const res = await native.youtubeSearch(ytQuery.trim());
                                    setYtBusy(null);
                                    if (!res.ok) {
                                      setError(res.error ?? "YouTube search failed");
                                      setYtHits([]);
                                      return;
                                    }
                                    setYtHits(
                                      (res.data as {
                                        id: string;
                                        title: string;
                                        channel?: string;
                                        duration?: number;
                                        url?: string;
                                      }[]) ?? []
                                    );
                                  })();
                                }
                              }}
                              placeholder="Song, artist, URL…"
                            />
                            <button
                              className="pill tiny"
                              disabled={!ytQuery.trim() || !!ytBusy}
                              onClick={() =>
                                void (async () => {
                                  setYtBusy("Searching…");
                                  setError(null);
                                  const res = await native.youtubeSearch(ytQuery.trim());
                                  setYtBusy(null);
                                  if (!res.ok) {
                                    setError(res.error ?? "YouTube search failed");
                                    setYtHits([]);
                                    return;
                                  }
                                  setYtHits(
                                    (res.data as {
                                      id: string;
                                      title: string;
                                      channel?: string;
                                      duration?: number;
                                      url?: string;
                                    }[]) ?? []
                                  );
                                })()
                              }
                            >
                              Search
                            </button>
                          </div>
                        </label>
                        {ytBusy && <p className="hint">{ytBusy}</p>}
                        {ytHits.length > 0 && (
                          <div className="yt-results">
                            {ytHits.map((hit) => (
                              <button
                                key={hit.id}
                                type="button"
                                className="yt-hit"
                                onClick={() =>
                                  void (async () => {
                                    setYtBusy(`Fetching audio…`);
                                    setError(null);
                                    const res = await native.youtubeLoadOntoNode(
                                      selected.id,
                                      hit.id,
                                      hit.title,
                                      hit.url
                                    );
                                    setYtBusy(null);
                                    if (!res.ok) setError(res.error ?? "Load failed");
                                    else if (res.data) applyGraph(res.data as GraphDocument);
                                  })()
                                }
                              >
                                <strong>{hit.title || hit.id}</strong>
                                <small className="muted">
                                  {[hit.channel, hit.duration ? `${Math.round(hit.duration)}s` : ""]
                                    .filter(Boolean)
                                    .join(" · ")}
                                </small>
                              </button>
                            ))}
                          </div>
                        )}
                        <p className="hint">
                          Audio only — requires{" "}
                          <button
                            type="button"
                            className="linkish"
                            onClick={() => void native.openExternal("https://github.com/yt-dlp/yt-dlp")}
                          >
                            yt-dlp
                          </button>{" "}
                          (and usually ffmpeg) on PATH.
                        </p>
                      </>
                    )}

                    {(() => {
                      const st = dspStatus[selected.id];
                      const dur = Math.max(0, st?.mediaDurationSec ?? 0);
                      const pos = Math.max(0, Math.min(dur, st?.mediaPositionSec ?? 0));
                      const playing = Boolean(st?.mediaPlaying ?? p.mediaPlaying);
                      const fmt = (sec: number) => {
                        const s = Math.max(0, Math.floor(sec));
                        const m = Math.floor(s / 60);
                        const r = s % 60;
                        return `${m}:${r.toString().padStart(2, "0")}`;
                      };
                      return (
                        <div className="media-transport">
                          <div className="media-transport-row">
                            <button
                              className="pill tiny"
                              disabled={!st?.loaded}
                              onClick={() =>
                                void updateParams({
                                  ...p,
                                  mediaPlaying: !playing,
                                  mediaSeekSec: !playing && dur > 0 && pos >= dur - 0.05 ? 0 : -1,
                                })
                              }
                            >
                              {playing ? "Pause" : "Play"}
                            </button>
                            <button
                              className="pill tiny"
                              disabled={!st?.loaded}
                              onClick={() =>
                                void updateParams({
                                  ...p,
                                  mediaPlaying: false,
                                  mediaSeekSec: 0,
                                })
                              }
                            >
                              Stop
                            </button>
                            <label className="check">
                              <input
                                type="checkbox"
                                checked={Boolean(p.mediaLoop)}
                                onChange={(e) =>
                                  void updateParams({ ...p, mediaLoop: e.target.checked, mediaSeekSec: -1 })
                                }
                              />
                              Loop
                            </label>
                          </div>
                          <label>
                            {fmt(pos)} / {fmt(dur)}
                            <input
                              type="range"
                              min={0}
                              max={Math.max(0.01, dur)}
                              step={0.01}
                              value={pos}
                              disabled={!st?.loaded || dur <= 0}
                              onChange={(e) => {
                                const seek = Number(e.target.value);
                                void updateParams({
                                  ...p,
                                  mediaSeekSec: seek,
                                  mediaPlaying: playing,
                                });
                              }}
                            />
                          </label>
                        </div>
                      );
                    })()}

                    <ParamNum
                      label="Level"
                      value={p.levelDb}
                      min={-24}
                      max={24}
                      step={0.1}
                      suffix="dB"
                      onChange={(v) => void updateParams({ ...p, levelDb: v, mediaSeekSec: -1 })}
                    />
                    <label className="check">
                      <input
                        type="checkbox"
                        checked={p.bypass}
                        onChange={(e) =>
                          void updateParams({ ...p, bypass: e.target.checked, mediaSeekSec: -1 })
                        }
                      />
                      Bypass
                    </label>
                  </div>
                )}

                {selected.type === "output" && (
                  <div className="inspector-grid">
                    <StereoVu title="OUT" peakL={io.outputPeakL ?? 0} peakR={io.outputPeakR ?? 0} />
                    <ParamNum
                      label="Level"
                      value={p.levelDb}
                      min={-24}
                      max={24}
                      step={0.1}
                      suffix="dB"
                      onChange={(v) => void updateParams({ ...p, levelDb: v })}
                    />
                    <ParamNum
                      label="Pan"
                      value={p.pan ?? 0}
                      min={-1}
                      max={1}
                      step={0.01}
                      onChange={(v) => void updateParams({ ...p, pan: v })}
                      onReset={() => void updateParams({ ...p, pan: 0 })}
                    />
                    {io.isStandalone && isHostOutput(selected) ? (
                      <>
                        <label>
                          Interface L out
                          <select
                            value={p.outputChannel ?? 0}
                            onChange={(e) =>
                              void updateParams({ ...p, outputChannel: Number(e.target.value) })
                            }
                          >
                            {Array.from({ length: Math.max(1, io.numOutputs || 2) }, (_, i) => (
                              <option key={i} value={i}>
                                Out {i + 1}
                              </option>
                            ))}
                          </select>
                        </label>
                        <label>
                          Interface R out
                          <select
                            value={p.outputChannelR ?? (p.outputChannel ?? 0) + 1}
                            onChange={(e) =>
                              void updateParams({ ...p, outputChannelR: Number(e.target.value) })
                            }
                          >
                            {Array.from({ length: Math.max(1, io.numOutputs || 2) }, (_, i) => (
                              <option key={i} value={i}>
                                Out {i + 1}
                              </option>
                            ))}
                          </select>
                        </label>
                      </>
                    ) : !io.isStandalone ? (
                      <p className="hint">Output returns to the DAW track — routing is controlled by the host.</p>
                    ) : null}
                  </div>
                )}

                {(selected.type === "nam" || selected.type === "ir") && (
                  <div className="inspector-grid">
                    {selected.params.imageUrl && (
                      <img className="inspector-thumb" src={selected.params.imageUrl} alt="" />
                    )}
                    <div className="inspector-meta">
                      {dspStatus[selected.id]?.error && (
                        <p className="error">Load failed: {dspStatus[selected.id].error}</p>
                      )}
                      {!dspStatus[selected.id]?.loaded && !selected.params.filePath && (
                        <p className="error">No model loaded</p>
                      )}
                      {!dspStatus[selected.id]?.loaded && !!selected.params.filePath && !dspStatus[selected.id]?.error && (
                        <p className="hint">Loading…</p>
                      )}
                      {selected.params.cabIncluded && (
                        <p className="cab-banner">Cab included</p>
                      )}
                    </div>
                    {selected.params.toneId && toneModels.length > 1 && (
                      <label>
                        Profile
                        <select
                          value={selected.params.modelId || ""}
                          onChange={(e) => {
                            const modelId = e.target.value;
                            void (async () => {
                              setBusy("Switching profile…");
                              const res = await native.toneSetNodeModel(
                                selected.id,
                                selected.params.toneId!,
                                modelId,
                                selected.type === "ir" ? "ir" : "nam",
                                selected.params.displayName,
                                selected.params.imageUrl,
                                selected.params.cabIncluded
                              );
                              setBusy(null);
                              if (!res.ok) setError(res.error ?? "profile switch failed");
                              else {
                                const data = res.data as { graph: GraphDocument; library?: LibraryItem[] };
                                applyGraph(data.graph);
                                if (data.library) setLibrary(data.library);
                              }
                            })();
                          }}
                        >
                          {toneModels.map((m) => (
                            <option key={m.id} value={m.id}>
                              {m.name || m.id}
                              {m.architecture ? ` · A${m.architecture}` : ""}
                              {m.size ? ` · ${m.size}` : ""}
                            </option>
                          ))}
                        </select>
                      </label>
                    )}
                    <ParamNum
                      label="Gain"
                      value={p.levelDb}
                      min={-24}
                      max={24}
                      step={0.1}
                      suffix="dB"
                      onChange={(v) => void updateParams({ ...p, levelDb: v })}
                    />
                    {selected.type === "nam" && (
                      <>
                        <ParamNum
                          label="Bass"
                          value={p.bassDb ?? 0}
                          min={-12}
                          max={12}
                          step={0.1}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, bassDb: v })}
                          onReset={() => void updateParams({ ...p, bassDb: 0 })}
                        />
                        <ParamNum
                          label="Mid"
                          value={p.midDb ?? 0}
                          min={-12}
                          max={12}
                          step={0.1}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, midDb: v })}
                          onReset={() => void updateParams({ ...p, midDb: 0 })}
                        />
                        <ParamNum
                          label="Treble"
                          value={p.trebleDb ?? 0}
                          min={-12}
                          max={12}
                          step={0.1}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, trebleDb: v })}
                          onReset={() => void updateParams({ ...p, trebleDb: 0 })}
                        />
                        <ParamNum
                          label="Slim"
                          value={p.slim}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, slim: v })}
                        />
                      </>
                    )}
                    {selected.type === "ir" && (
                      <ParamNum
                        label="Mix"
                        value={p.mix}
                        min={0}
                        max={1}
                        step={0.01}
                        onChange={(v) => void updateParams({ ...p, mix: v })}
                      />
                    )}
                    <label className="check">
                      <input
                        type="checkbox"
                        checked={p.bypass}
                        onChange={(e) => void updateParams({ ...p, bypass: e.target.checked })}
                      />
                      Bypass
                    </label>
                    <button
                      className="pill tiny"
                      onClick={() => void native.pickFileForNode(selected.id, selected.type)}
                    >
                      Load file
                    </button>
                  </div>
                )}

                {(selected.type === "split" || selected.type === "merge") && (
                  <div className="inspector-grid">
                    <ParamNum
                      label="Balance"
                      value={p.mix}
                      min={0}
                      max={1}
                      step={0.01}
                      onChange={(v) => void updateParams({ ...p, mix: v })}
                      onReset={() => void updateParams({ ...p, mix: 0.5 })}
                    />
                    <label className="check">
                      <input
                        type="checkbox"
                        checked={p.bypass}
                        onChange={(e) => void updateParams({ ...p, bypass: e.target.checked })}
                      />
                      Bypass
                    </label>
                  </div>
                )}

                {selected.type === "fx" && (
                  <div className="inspector-grid">
                    {!(
                      p.fxId === "gate" ||
                      p.fxId === "compressor" ||
                      selected.params.displayName.toLowerCase().includes("gate") ||
                      selected.params.displayName.toLowerCase().includes("compress")
                    ) && (
                      <ParamNum
                        label="Mix"
                        value={p.mix}
                        min={0}
                        max={1}
                        step={0.01}
                        onChange={(v) => void updateParams({ ...p, mix: v })}
                      />
                    )}
                    {(p.fxId === "gate" || selected.params.displayName.toLowerCase().includes("gate")) && (
                      <>
                        <ParamNum
                          label="Threshold"
                          value={p.thresholdDb ?? -50}
                          min={-80}
                          max={-10}
                          step={0.5}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, thresholdDb: v })}
                        />
                        <ParamNum
                          label="Release"
                          value={p.releaseMs ?? 150}
                          min={10}
                          max={1000}
                          step={5}
                          suffix="ms"
                          onChange={(v) => void updateParams({ ...p, releaseMs: v })}
                        />
                      </>
                    )}
                    {(p.fxId === "compressor" ||
                      selected.params.displayName.toLowerCase().includes("compress")) && (
                      <>
                        <ParamNum
                          label="Threshold"
                          value={p.thresholdDb ?? -18}
                          min={-60}
                          max={0}
                          step={0.5}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, thresholdDb: v })}
                        />
                        <ParamNum
                          label="Ratio"
                          value={p.ratio ?? 4}
                          min={1}
                          max={20}
                          step={0.1}
                          onChange={(v) => void updateParams({ ...p, ratio: v })}
                        />
                        <ParamNum
                          label="Attack"
                          value={p.attackMs ?? 10}
                          min={0.1}
                          max={100}
                          step={0.1}
                          suffix="ms"
                          onChange={(v) => void updateParams({ ...p, attackMs: v })}
                        />
                        <ParamNum
                          label="Release"
                          value={p.releaseMs ?? 100}
                          min={5}
                          max={1000}
                          step={1}
                          suffix="ms"
                          onChange={(v) => void updateParams({ ...p, releaseMs: v })}
                        />
                        <ParamNum
                          label="Makeup"
                          value={p.makeupDb ?? 0}
                          min={0}
                          max={24}
                          step={0.1}
                          suffix="dB"
                          onChange={(v) => void updateParams({ ...p, makeupDb: v })}
                        />
                        <ParamNum
                          label="Mix"
                          value={p.mix}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, mix: v })}
                        />
                      </>
                    )}
                    {(p.fxId === "delay" ||
                      p.fxId === "pingpong" ||
                      selected.params.displayName.toLowerCase().includes("delay") ||
                      selected.params.displayName.toLowerCase().includes("ping")) && (
                      <>
                        <label>
                          Mode
                          <select
                            value={(p.fxMode || "digital").toLowerCase()}
                            onChange={(e) => void updateParams({ ...p, fxMode: e.target.value })}
                          >
                            <option value="digital">Digital</option>
                            <option value="analogue">Analogue</option>
                            <option value="tape">Tape</option>
                          </select>
                        </label>
                        <ParamNum
                          label="Time"
                          value={p.delayMs ?? 350}
                          min={20}
                          max={1500}
                          step={1}
                          decimals={0}
                          suffix="ms"
                          onChange={(v) => void updateParams({ ...p, delayMs: v })}
                        />
                        <ParamNum
                          label="Feedback"
                          value={p.feedback ?? 0.35}
                          min={0}
                          max={0.95}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, feedback: v })}
                        />
                      </>
                    )}
                    {(p.fxId === "chorus" || selected.params.displayName.toLowerCase().includes("chorus")) && (
                      <>
                        <ParamNum
                          label="Rate"
                          value={p.rateHz ?? 0.8}
                          min={0.05}
                          max={5}
                          step={0.01}
                          suffix="Hz"
                          onChange={(v) => void updateParams({ ...p, rateHz: v })}
                        />
                        <ParamNum
                          label="Depth"
                          value={p.depth ?? 0.35}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, depth: v })}
                        />
                        <ParamNum
                          label="Feedback"
                          value={p.feedback ?? 0.12}
                          min={0}
                          max={0.7}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, feedback: v })}
                        />
                      </>
                    )}
                    {(p.fxId === "reverb" ||
                      (!p.fxId && selected.params.displayName.toLowerCase().includes("reverb"))) && (
                      <>
                        <label>
                          Type
                          <select
                            value={(p.fxMode || "room").toLowerCase()}
                            onChange={(e) => void updateParams({ ...p, fxMode: e.target.value })}
                          >
                            <option value="room">Room</option>
                            <option value="plate">Plate</option>
                            <option value="spring">Spring</option>
                          </select>
                        </label>
                        <ParamNum
                          label="Size"
                          value={p.roomSize ?? 0.45}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, roomSize: v })}
                        />
                        <ParamNum
                          label="Damping"
                          value={p.damping ?? 0.4}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, damping: v })}
                        />
                        <ParamNum
                          label="Width"
                          value={p.width ?? 1}
                          min={0}
                          max={1}
                          step={0.01}
                          onChange={(v) => void updateParams({ ...p, width: v })}
                        />
                      </>
                    )}
                    <ParamNum
                      label="Level"
                      value={p.levelDb}
                      min={-24}
                      max={24}
                      step={0.1}
                      suffix="dB"
                      onChange={(v) => void updateParams({ ...p, levelDb: v })}
                    />
                    <label className="check">
                      <input
                        type="checkbox"
                        checked={p.bypass}
                        onChange={(e) => void updateParams({ ...p, bypass: e.target.checked })}
                      />
                      Bypass
                    </label>
                  </div>
                )}
              </>
            )}
          </div>
          </div>
        </div>

        {toneOpen && (
          <aside className="tone-frame">
            <div
              className="tone-resize"
              onPointerDown={(e) => {
                e.preventDefault();
                toneDragRef.current = { startX: e.clientX, startW: toneW };
                try {
                  (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
                } catch {
                  /* ignore */
                }
              }}
              onPointerMove={(e) => {
                if (!toneDragRef.current) return;
                const dx = toneDragRef.current.startX - e.clientX; // drag left = wider
                const next = Math.max(280, Math.min(900, toneDragRef.current.startW + dx));
                setToneW(next);
              }}
              onPointerUp={() => {
                if (!toneDragRef.current) return;
                toneDragRef.current = null;
                setToneW((w) => {
                  try {
                    localStorage.setItem("namplifier.toneW", String(w));
                  } catch {
                    /* ignore */
                  }
                  return w;
                });
              }}
            />
            <div className="panel-head">
              <div className="tone-brand-head">
                <img className="tone-logo" src={tone3000Logo} alt="TONE3000" />
                <h2 className="tone-sr-only">Tone3000</h2>
              </div>
              <div className="row tight">
                <button
                  className="pill tiny"
                  onClick={() =>
                    setToneFiltersOpen((v) => {
                      const next = !v;
                      try {
                        localStorage.setItem("namplifier.toneFiltersOpen", next ? "1" : "0");
                      } catch {
                        /* ignore */
                      }
                      return next;
                    })
                  }
                >
                  {toneFiltersOpen ? "Hide tools" : "Tools"}
                </button>
                <button className="pill tiny" onClick={() => setToneOpen(false)}>
                  Close
                </button>
              </div>
            </div>
            <div className={`tone-tools ${toneFiltersOpen ? "" : "collapsed"}`}>
            <div className={`auth-banner ${signedIn ? "signed-in" : "signed-out"}`}>
              <span className={`auth-dot ${signedIn ? "on" : "off"}`} aria-hidden />
              <div className="auth-copy">
                <strong>{signedIn ? "Signed in" : "Signed out"}</strong>
              </div>
            </div>
            <div className="row">
              <button
                className={`pill primary ${signedIn ? "disabled" : ""}`}
                disabled={signedIn || loginWaiting}
                onClick={async () => {
                  setError(null);
                  setLoginWaiting(true);
                  const r = await native.toneBeginLogin();
                  if (!r.ok) {
                    setLoginWaiting(false);
                    setError(r.error ?? "Could not start login");
                  }
                }}
              >
                {loginWaiting ? "Waiting…" : "Sign in"}
              </button>
              <button
                className="pill"
                disabled={!signedIn}
                onClick={() => void native.toneLogout().then(refresh)}
              >
                Sign out
              </button>
            </div>
            {loginWaiting && <p className="hint">Waiting for browser…</p>}

            <div className="chips" style={{ marginTop: 10 }}>
              {(
                [
                  ["browse", "Browse"],
                  ["favorites", "Favorites"],
                  ["created", "Yours"],
                  ["downloaded", "Downloaded"],
                ] as const
              ).map(([id, label]) => (
                <button
                  key={id}
                  className={toneBrowse === id ? "chip active" : "chip"}
                  onClick={() => {
                    if (id === "browse") void searchTones(1);
                    else void loadToneCollection(id, 1);
                  }}
                >
                  {label}
                </button>
              ))}
            </div>

            {toneBrowse === "browse" && (
              <>
                <div className="chips">
                  {(
                    [
                      ["nam", "NAM"],
                      ["ir", "IR"],
                      ["", "All"],
                    ] as const
                  ).map(([id, label]) => (
                    <button
                      key={label}
                      className={toneFormat === id ? "chip active" : "chip"}
                      onClick={() => setToneFormat(id)}
                    >
                      {label}
                    </button>
                  ))}
                </div>

                <div className="field">
                  <label>Search</label>
                  <input
                    value={toneQuery}
                    onChange={(e) => setToneQuery(e.target.value)}
                    placeholder="fender, plexi, diezel…"
                    onKeyDown={(e) => e.key === "Enter" && void searchTones(1)}
                  />
                </div>

                <div className="field compact">
                  <label>Sort</label>
                  <select value={toneSort} onChange={(e) => setToneSort(e.target.value)}>
                    <option value="trending">Trending</option>
                    <option value="newest">Newest</option>
                    <option value="oldest">Oldest</option>
                    <option value="downloads-all-time">Most downloaded</option>
                    <option value="best-match">Best match</option>
                  </select>
                </div>

                {toneFormat !== "ir" && (
                  <>
                    <div className="chips tight">
                      <span className="chip-label">Gear</span>
                      {(
                        [
                          ["", "Any"],
                          ["amp", "Amp"],
                          ["amp-cab", "Amp+Cab"],
                          ["cab", "Cab"],
                          ["pedal", "Pedal"],
                          ["outboard", "Outboard"],
                          ["space", "Space"],
                          ["experimental", "Experimental"],
                        ] as const
                      ).map(([id, label]) => (
                        <button
                          key={label}
                          className={toneGear === id ? "chip active" : "chip"}
                          onClick={() => setToneGear(id)}
                        >
                          {label}
                        </button>
                      ))}
                    </div>

                    <div className="chips tight">
                      <span className="chip-label">Arch</span>
                      {(
                        [
                          ["2", "A2"],
                          ["1", "A1"],
                          ["custom", "Custom"],
                          ["", "Any"],
                        ] as const
                      ).map(([id, label]) => (
                        <button
                          key={label}
                          className={toneArch === id ? "chip active" : "chip"}
                          onClick={() => setToneArch(id)}
                        >
                          {label}
                        </button>
                      ))}
                    </div>

                    <div className="chips tight">
                      <span className="chip-label">Size</span>
                      {(
                        [
                          ["", "Any"],
                          ["standard", "Standard"],
                          ["lite", "Lite"],
                          ["feather", "Feather"],
                          ["nano", "Nano"],
                        ] as const
                      ).map(([id, label]) => (
                        <button
                          key={label}
                          className={toneSize === id ? "chip active" : "chip"}
                          onClick={() => setToneSize(id)}
                        >
                          {label}
                        </button>
                      ))}
                    </div>

                    <label className="check tone-check">
                      <input
                        type="checkbox"
                        checked={toneCalibrated}
                        onChange={(e) => setToneCalibrated(e.target.checked)}
                      />
                      Calibrated only
                    </label>
                  </>
                )}

                <div className="row">
                  <button className="pill primary" onClick={() => void searchTones(1)}>
                    Search
                  </button>
                  <button
                    className="pill"
                    onClick={() => {
                      setToneQuery("");
                      setToneSort("trending");
                      setToneGear("");
                      setToneArch("2");
                      setToneSize("");
                      setToneCalibrated(false);
                      setToneFormat("nam");
                      void searchTones(1);
                    }}
                  >
                    Reset
                  </button>
                </div>
              </>
            )}

            {toneBrowse !== "browse" && (
              <div className="row" style={{ marginTop: 8 }}>
                <button className="pill" onClick={() => void loadToneCollection(toneBrowse, 1)}>
                  Refresh
                </button>
              </div>
            )}
            </div>

            <div className="tone-page-bar">
              <button
                className="pill tiny"
                disabled={tonePage <= 1 || !!busy}
                onClick={() => void reloadTonePage(tonePage - 1)}
              >
                ← Prev
              </button>
              <span className="hint">Page {tonePage}</span>
              <button
                className="pill tiny"
                disabled={tones.length < TONE_PAGE_SIZE || !!busy}
                onClick={() => void reloadTonePage(tonePage + 1)}
              >
                Next →
              </button>
            </div>

            <div className="tone-list lib-scroll">
              {tones.map((t) => {
                const starred = favoritedIds.has(t.id) || Boolean(t.favorited);
                return (
                  <div
                    key={t.id}
                    className="tone-card lib-main"
                    draggable
                    onDragStart={(e) => {
                      e.dataTransfer.setData(
                        "application/x-namplifier-tone",
                        JSON.stringify({
                          id: t.id,
                          name: t.name,
                          format: t.format || toneFormat || "nam",
                          imageUrl: t.imageUrl,
                          cabIncluded: t.cabIncluded,
                        })
                      );
                      e.dataTransfer.effectAllowed = "copy";
                    }}
                    onClick={() => void useToneItem(t)}
                  >
                    <div className="tone-card-top">
                      {t.imageUrl ? (
                        <img className="tone-thumb" src={t.imageUrl} alt="" loading="lazy" />
                      ) : (
                        <div className="tone-thumb placeholder" />
                      )}
                      <div className="tone-meta">
                        <strong>{t.name || "Untitled"}</strong>
                        <small>
                          {t.creator || "unknown"}
                          {t.gear ? ` · ${t.gear}` : ""}
                          {t.gearType ? ` · ${t.gearType}` : ""}
                          {t.format ? ` · ${t.format}` : ""}
                          {t.cabIncluded ? " · cab included" : ""}
                          {(t.modelsCount ?? 0) > 1 ? ` · ${t.modelsCount} profiles` : ""}
                        </small>
                        <div className="tone-stats">
                          <span title="Downloads">↓ {t.downloads ?? 0}</span>
                          <span title="Favorites">★ {t.favorites ?? 0}</span>
                          {t.cabIncluded && <span className="cab-tag">cab in</span>}
                        </div>
                      </div>
                      <button
                        className={`pill tiny star-btn ${starred ? "starred" : ""}`}
                        title={starred ? "Unfavorite" : "Favorite"}
                        onClick={(e) => {
                          e.stopPropagation();
                          void toggleFavorite(t);
                        }}
                      >
                        {starred ? "★" : "☆"}
                      </button>
                    </div>
                    {t.description && <p className="tone-desc">{t.description}</p>}
                    <div className="row tight" onClick={(e) => e.stopPropagation()}>
                      <button className="pill tiny" onClick={() => void addToneToLibrary(t)}>
                        Library
                      </button>
                      <button className="pill tiny primary" onClick={() => void useToneItem(t)}>
                        {selected &&
                        ((selected.type === "nam" && (t.format || toneFormat) !== "ir") ||
                          (selected.type === "ir" && (t.format || toneFormat) === "ir"))
                          ? "Apply"
                          : "Add to graph"}
                      </button>
                    </div>
                  </div>
                );
              })}
              {tones.length === 0 && !busy && (
                <p className="hint">{toneBrowse === "browse" ? "No results" : "Nothing here yet"}</p>
              )}
            </div>

            <footer className="tone-powered">
              <button
                type="button"
                className="tone-powered-link"
                onClick={() => void native.openExternal("https://www.tone3000.com")}
                title="Open tone3000.com"
              >
                <span>Powered by TONE3000</span>
                <img src={tone3000Logo} alt="" />
              </button>
            </footer>
          </aside>
        )}
      </div>

      {presetsOpen && (
        <div className="preset-pop">
          <div className="panel-head">
            <h2>Presets</h2>
            <button className="pill tiny" onClick={() => setPresetsOpen(false)}>
              Close
            </button>
          </div>
          <div className="field">
            <label>Save current chain as</label>
            <input value={presetName} onChange={(e) => setPresetName(e.target.value)} />
          </div>
          <button
            className="pill primary"
            onClick={async () => {
              const r = await native.savePreset(presetName);
              if (!r.ok) setError(r.error ?? "save failed");
              else void refresh();
            }}
          >
            Save
          </button>
          <div className="tone-list">
            {presets.map((name) => (
              <div key={name} className="row">
                <button
                  className="pill"
                  onClick={async () => {
                    const r = await native.loadPreset(name);
                    if (r.ok) applyGraph(r.data as GraphDocument);
                    else setError(r.error ?? "load failed");
                  }}
                >
                  {name}
                </button>
                <button
                  className="pill tiny danger"
                  onClick={() => void native.deletePreset(name).then(refresh)}
                >
                  ×
                </button>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
