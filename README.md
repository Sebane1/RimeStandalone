# RIME Standalone
I kinda thought it was a bummer that I wasnt able to use Neumann RIME standalone on windows. I wanted it to work with Youtube, Media Players, games with surround sound support, etc.
This experimental project allows the Nuemann RIME VST3 to process audio from windows via APO, with secondary input from VB-CABLE output for surround sound.

You will need [Neumann RIME](https://www.neumann.com/rime) installed on the default C drive path, or have to set a custom path if it is elsewhere.
This project allows RIME to be used without a DAW. You will still need to own RIME

## Quick Install

1. Download the latest `RIME_Standalone_Release.zip` and extract it to a folder.
2. Double-click `rime_setup.exe` (it will ask for Administrator privileges).
3. Select your physical headphones from the dropdown list and click **Install**.
4. **That's it.** The installer will automatically:
   - Silently install the VB-Audio Virtual Cable driver.
   - Inject the RIME APO directly into your selected headphones.
   - Setup a silent background startup task.
   - Launch the real-time bridge.

## Surround Sound support
Surround support requires the use of VB-CABLE and the Input and Output must be set to 8 channel 16 bit 48000 hz.
Note, that not all games appear to support funnelling surround sound through VB-CABLE.

Games like Halo MCC and Pragmata work natively. However, games like Mirrors Edge 2009 and NieR Replicant did not despite having advertised surround support.
This is likely because VB-Cable simply advertises 8-channel audio rather than specifically identifying as a "Surround 7.1" endpoint.

### Zero-Config Virtual Ingestion
RIME Standalone features a fully automated, plug-and-play architecture. 
Whenever the bridge launches, it automatically queries the Windows Registry to pinpoint exactly where the APO is installed. It then scans all active capture endpoints on your system. If you route audio through **VB-Cable**, or just use the **Native APO** directly, the bridge instantly detects the active streams and additively mixes them together into your headphones in real-time. No manual pipeline configuration is required!

## Uninstall

Simply run `rime_setup.exe` again and click the **Uninstall** button. It will cleanly remove the APO from your headphones, unregister all COM servers, and remove the background startup tasks.
## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File Install.ps1 -Uninstall
```
## Surround Sound support
Surround support requires the use of VB-CABLE and the Input and Output must be set to 8 channel 16 bit 48000 hz.
Note, that not all games appear to support funnelling surround sound through VB-CABLE.

Game like Halo MCC, FFXIV and Pragmata seemed to work. But games like Mirrors Edge 2009, and NieR Replicant ver.1.22474487139… did not despite having advertised surround support.
I suspect its because VB-Cable doesnt explicitly say its a surround sound device. It simply supports up to 16 channel audio. Different games query this audio device info differently.

Long term may have to make our own passthrough driver that advertises 5.1/7.1 surround sound input in addition to the 16 channel audio support.

## Head Tracking
I have added support to automatically read head tracking data from SlimeVR server and forward it to RIME. SlimeVR uses IMU based trackers that can be easily strapped to the top of headphones or other joints of the body.

## Configuration

Edit `%APPDATA%\RIME Standalone\config.json`:

```json
{
  "pluginPath": "C:\\Program Files\\Common Files\\VST3\\Neumann RIME.vst3\\Contents\\x86_64-win\\Neumann RIME.vst3",
  "sampleRate": 48000,
  "outputGainDb": 4.0
}
```

| Setting | Default | Description |
|---------|---------|-------------|
| `pluginPath` | *(Neumann RIME default)* | Path to the VST3 plugin binary |
| `sampleRate` | `48000` | Sample rate in Hz |
| `outputGainDb` | `4.0` | Output gain compensation in dB (compensates for volume reduction from spatial processing) |

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Windows Audio Engine (audiodg.exe)                     │
│  ┌────────────┐    Shared Memory IPC    ┌────────────┐  │
│  │  rime_apo   │◄──────────────────────►│rime_bridge │  │
│  │  (APO DLL)  │  Ring Buffers + Events │ (VST3 Host)│  │
│  └────────────┘                         └────────────┘  │
│        │                                      │         │
│   Audio In/Out                          RIME VST3       │
│   (per-device)                          Processing      │
└─────────────────────────────────────────────────────────┘
```

- **rime_apo.dll** — Loaded by Windows into `audiodg.exe`. Intercepts audio and pipes it through shared memory. Only one instance claims the IPC channel; others passthrough.
- **rime_bridge.exe** — User-space process that hosts the RIME VST3 plugin. Reads audio from the APO, processes it, and sends it back. Runs with MMCSS "Pro Audio" thread priority.

## Latency

The round-trip latency is ~10ms (one WASAPI period at 48kHz). Processing time is ~0.5ms per block.

## Requirements

- Windows 10/11 (64-bit)
- [Neumann RIME VST3 plugin](https://www.neumann.com/rime) installed
- Administrator privileges (for APO installation)
- Visual Studio 2022 Build Tools (for building from source)
