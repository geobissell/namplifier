# Namplifier

NAM / NAM A2 amp sim VST3 with freeform routing, Tone3000 browse, a local Library, IR convolution, and custom presets.

## UI layout

- **Library (left)** — scrollable NAM profiles, IRs, and future non-NAM FX. Add from Tone3000 or local files; click to drop onto the graph (or apply to the selected block).
- **Routing canvas (center)** — always visible Quad Cortex-style graph. Never navigates away to a full-screen Tone3000 page.
- **Tone3000 (right dock)** — opens as a side frame from the top bar. Search / favorites, then **+ Library** (does not replace your chain view).
- **Presets** — floating popup for saving/loading full graphs.

## Features (MVP)

- Freeform block graph (Input / NAM / IR / Split / Merge / Output)
- Local `.nam` (A1 + A2 via NeuralAmpModelerCore) and `.wav` IR loading
- Tone3000 OAuth + in-plugin search + favorites + download into Library cache
- User presets under `Documents/Namplifier/Presets`
- Library persisted at `Documents/Namplifier/library.json`
- VST3 + Standalone (Windows)

## Requirements

- Windows 10/11
- CMake 3.24+
- Visual Studio 2022/2026 with C++ desktop workload
- Node.js 20+ (UI)
- WebView2 Runtime + NuGet package `Microsoft.Web.WebView2` 1.0.2903.40

## Tone3000 attribution

Namplifier integrates the [TONE3000](https://www.tone3000.com) API for browsing and downloading tones.

**Powered by TONE3000** — see [API terms](https://www.tone3000.com/api/terms). Commercial use requires Tone3000’s approval before public launch. The in-app Tone3000 panel shows this attribution and links to [tone3000.com](https://www.tone3000.com). The TONE3000 logo is used unmodified from their brand assets.

## Tone3000 API key

Tone3000 keys must **never** be committed. Use a local env file or CI secret.

1. Copy `.env.example` → `.env.local` and paste your publishable key.
2. Or set the env var / CMake cache entry:

```bat
set Namplifier_TONE3000_KEY=t3k_pk_...
```

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DNamplifier_TONE3000_KEY=%Namplifier_TONE3000_KEY%
```

Register redirect URI `http://localhost/namplifier/oauth/callback` in Tone3000 API settings.

### GitHub Actions secret

For automated builds, add repository secret **`Namplifier_TONE3000_KEY`**  
(Settings → Secrets and variables → Actions). The workflow on `main` / `master` injects it at configure time only — it is not written into the repo.

## Build

```bat
cd ui
npm install
npm run build

cd ..
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target Namplifier_Standalone Namplifier_VST3
```

Optional ASIO: see `third_party/asiosdk/README.md`.

Artefacts:

- `build/Namplifier_artefacts/Release/Standalone/Namplifier.exe`
- `build/Namplifier_artefacts/Release/VST3/Namplifier.vst3`

Shareable Standalone package layout:

```
Namplifier.exe
ui/dist/...
```

## CI / Releases

Pushing or merging to `main` / `master` runs `.github/workflows/build.yml`:

| Platform | Artefacts |
|---|---|
| **Windows** | Standalone `.exe` + **VST3** |
| **macOS** (universal arm64 + x86_64) | Standalone `.app` + **VST3** + **AU** |

- **Actions artefacts** — every push and PR (30-day retention).
- **GitHub Releases** — on push/merge to `main` / `master`, a release (`build-<run>`) is published with all platform zips. Newest is marked **Latest**.

macOS builds are unsigned (Gatekeeper: right-click → Open, or `xattr -cr` on the app/plugin).

Add repository secret **`Namplifier_TONE3000_KEY`** so CI builds include Tone3000 support.

## License

Namplifier is licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0), which matches JUCE’s open-source licence path.

Third-party code (NeuralAmpModelerCore, AudioDSPTools, JUCE, etc.) remains under its own licences — see `third_party/` and JUCE docs.

Tone discovery uses the TONE3000 API (**Powered by TONE3000**). See [tone3000.com](https://www.tone3000.com) and their [API terms](https://www.tone3000.com/api/terms). API/catalog use is governed by those terms, not this licence.
