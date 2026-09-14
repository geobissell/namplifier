/** Minimal JUCE 8 native-function client (avoids full juce frontend package). */
export function installJuceNativeShim() {
  const w = window as unknown as {
    getNativeFunction?: (name: string) => (...args: unknown[]) => Promise<unknown>;
    __JUCE__?: {
      backend?: {
        invoke?: (name: string, params: unknown) => Promise<unknown>;
        nativeFunction?: (name: string, args: unknown[]) => Promise<unknown>;
      };
      initialisationData?: unknown;
    };
  };

  if (typeof w.getNativeFunction === "function") return;

  w.getNativeFunction = (name: string) => {
    return async (...args: unknown[]) => {
      const backend = w.__JUCE__?.backend;
      if (!backend) throw new Error("JUCE backend missing");

      // JUCE 8 resource-provider integration exposes invoke-style helpers once injected.
      const anyBackend = backend as Record<string, unknown>;
      if (typeof anyBackend.invoke === "function") {
        return (anyBackend.invoke as (n: string, a: unknown) => Promise<unknown>)(name, args[0] ?? {});
      }

      // Fallback: postMessage protocol used by some JUCE builds
      return new Promise((resolve, reject) => {
        const promiseId = `p_${Math.random().toString(36).slice(2)}`;
        const handler = (event: MessageEvent) => {
          const data = event.data;
          if (!data || data.promiseId !== promiseId) return;
          window.removeEventListener("message", handler);
          if (data.error) reject(new Error(String(data.error)));
          else resolve(data.result);
        };
        window.addEventListener("message", handler);
        try {
          (window as unknown as { __JUCE__: { backend: { emitEvent: (a: string, b: unknown) => void } } }).__JUCE__.backend.emitEvent(
            "__juce__complete",
            { promiseId, name, args }
          );
        } catch (e) {
          window.removeEventListener("message", handler);
          reject(e);
        }
      });
    };
  };
}
