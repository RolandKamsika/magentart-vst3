# MRT2 Remote

A Windows VST3 instrument that plays [Magenta RealTime 2](https://magenta.tensorflow.org/mrt2) from a DAW such as Ableton Live.

Google's own MRT2 plugin is a macOS Audio Unit for Apple Silicon only, and the model needs a GPU with at least 12 GB. So this plugin doesn't run the model itself. It runs on a GPU server and the plugin talks to it over the network:

```
Ableton ──MIDI──▶ MRT2 Remote (VST3) ──WebSocket: held notes + settings──▶ Magenta jam server on enki (magenta.yaml)
        ◀─audio──                     ◀──────────── 48 kHz stereo audio ────
```

The server is the Magenta RealTime jam app, run by [`magenta.yaml`](../magenta.yaml) from the root of this repo. The plugin uses the same protocol as that app's own web page, so the server needs no changes.

## Requirements

- **Windows 10 or 11 (64-bit)** with a VST3 host, such as Ableton Live 11 or later.
- **The project sample rate set to 48 kHz.** The model only produces 48 kHz, and the plugin is silent at any other rate.
- **Magenta running on the server.** On enki, `~/docker/magenta/stack/magenta.sh up` starts it. It also stops `llama-swap`, because both can't fit on the GPUs at once. `magenta.sh down` reverses it.
- **A wired or stable network connection** to the server.

## Install

Unzip `release/MRT2-Remote-<version>-win64.zip` into `C:\Program Files\Common Files\VST3\`, which needs admin rights. That gives you `C:\Program Files\Common Files\VST3\MRT2 Remote.vst3`.

In Ableton, turn on *Preferences → Plug-Ins → Use VST3 Plug-in System Folders*, click **Rescan**, and set *Preferences → Audio → Sample Rate* to **48000**.

## Use

1. Start Magenta on the server: `magenta.sh up`. It's ready after about a minute.
2. Drop **MRT2 Remote** onto a MIDI track. The status line at the bottom of the plugin window should say **streaming**.
3. Type a style into **Style prompt**, such as `funky bass and drums` or `ambient synth pads`, and press Enter.
4. Play notes or chords. Held notes steer the music, and with no notes held the model keeps playing on its own, unless **Solo** is on.
5. To record the output, arm an audio track with *Audio From* set to the MRT2 Remote track.

| Control | What it does |
|---|---|
| Server | Server address. The default is `http://enki.local:7860`; use `http://192.168.50.205:7860` if `.local` names don't resolve |
| Style prompt | Text describing the style the model plays in |
| Temperature | Higher is wilder and more varied; lower is more predictable |
| Style strength | How closely it follows the style prompt |
| Note strength | How closely it follows the held notes |
| Gain | Output volume |
| Server buffer | Audio the server keeps queued: 150, 300 or 500 ms. Raise it if you hear dropouts |
| Solo | Plays only the held notes, and mostly goes quiet when none are held |
| Drums | Turn off to leave out percussion |

The server address, prompt and all controls are saved with the Ableton project.

| Status | Meaning |
|---|---|
| streaming | Connected and receiving audio |
| can't reach … | Magenta isn't running, or the server address is wrong. The plugin retries every 2 s |
| set the project sample rate to 48 kHz | The project isn't at 48 kHz |
| disconnected / connection error | The connection dropped; it reconnects by itself |

## Latency

Measured from a note entering the plugin to sound leaving it, with the 150 ms server buffer on a wired LAN: **about 320 ms median (267–352 ms)**. Ableton's audio buffer and your sound card output add to that.

Most of it comes from two places:
- **Queued audio:** about 240 ms of audio is queued ahead in the plugin, which protects against dropouts.
- **Batches:** the model generates audio 120 ms at a time.

The 300 ms and 500 ms buffer settings add about 0.15 s and 0.35 s. The model also fades towards the held notes rather than starting them like a synth, so notes can feel later than the measured figure.

## Limitations

- **No tempo or transport sync.** The model has no tempo or beat input, so it can't follow the DAW's tempo or bar lines.
- **No latency reporting.** The plugin doesn't report its delay to the DAW, so line up recorded clips by hand.
- **Plays continuously while connected,** unless Solo is on.
- **Needs the server.** With Magenta not running, the plugin is silent.
- **One plugin instance per server.** Each instance opens its own session, and a second one on the same GPU will probably fall behind.
- **Only the `mrt2_small` model.** `magenta.yaml` only loads the small model.

## Build

You need Visual Studio 2022 or later with the **Desktop development with C++** workload, which includes CMake. JUCE 8.0.4 and IXWebSocket 11.4.5 are downloaded at configure time.

Run this from the repo root:

```powershell
$cmake = "<Visual Studio>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S mrt2-vst -B mrt2-vst\build -G "Visual Studio 18 2026" -A x64   # or "Visual Studio 17 2022"
& $cmake --build mrt2-vst\build --config Release --target MRT2Remote_VST3 MRT2Harness
```

The output goes to `build\MRT2Remote_artefacts\Release\VST3\MRT2 Remote.vst3`. To package a release:

```powershell
Compress-Archive -Path "mrt2-vst\build\MRT2Remote_artefacts\Release\VST3\MRT2 Remote.vst3" `
                 -DestinationPath mrt2-vst\release\MRT2-Remote-0.1.0-win64.zip -Force
```

`build/` and `release/` are not committed.

## Tests

`MRT2Harness` loads the built `.vst3` the way a DAW does and drives it in real time against the running server.

```powershell
$h   = "mrt2-vst\build\MRT2Harness_artefacts\Release\MRT2Harness.exe"
$vst = "$PWD\mrt2-vst\build\MRT2Remote_artefacts\Release\VST3\MRT2 Remote.vst3"
& $h $vst 70                    # stream check: holds a chord and fails if over 10% of blocks are silent
& $h $vst latency latency.wav   # latency: 6 chord on/off trials in solo mode, and saves the audio
```

Both need Magenta running on the server. The stream check runs for 70 seconds here because the server restarts its generation loop every 55 seconds, and the test should cross that point.

## How it works

| File | Contents |
|---|---|
| `Source/PluginProcessor.*` | Audio processing, networking and saved settings |
| `Source/PluginEditor.*` | The plugin window |
| `tests/Harness.cpp` | The test host |

**Session**
- The plugin calls `POST /gradio_api/call/start` with `{"data":[session_id]}`, then opens `ws://<server>/ws?session_id=…`.
- This happens on a background thread, so a slow or missing server never blocks the DAW.

**Plugin to server**
- A JSON `{"type":"set", …}` message carries the held notes, prompt and settings.
- A 10 ms timer sends it whenever something changes, so the audio thread never touches the network.
- After each audio packet, the plugin sends `{"type":"buffer","lead":ms}` with how much audio it has queued, which lets the server slow down.

**Server to plugin**
- Each binary frame is a 4-byte float with the server's frame time, followed by interleaved 16-bit stereo at 48 kHz.
- The frames go into a lock-free one-second buffer that the audio thread reads. If the buffer runs dry, the rest of the block is silent.

**Reconnecting**
- The server ends a session when its socket closes, so a reconnect always starts a new session.

## Licences

This plugin uses JUCE, which is dual-licensed under the AGPLv3 or JUCE's commercial licences. IXWebSocket is BSD-3-Clause. Distributing builds of this plugin to other people has to meet JUCE's licence terms, so check them before sharing it. The model and server code are Google's, under Apache-2.0, and the model weights have their own terms on Hugging Face.
