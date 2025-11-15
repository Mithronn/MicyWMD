# 🎤 MicyWMD (Virtual Microphone Driver)

**MicyWMD** is an open-source Windows *virtual microphone* (loopback/virtual audio capture driver) based on the Microsoft [**SimpleAudioSample**](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/simpleaudiosample) reference implementation. It exposes a virtual audio capture device to Windows applications so audio generated or routed by user-space components can appear as microphone input to other apps (VoIP, streaming, games, etc.).

## Table of contents

- [Features](#features)
- [Why MicyWMD?](#why-micywmd)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Installing & Testing](#installing--testing)
- [Usage](#usage)
- [Developer notes](#developer-notes)
- [Contributing](#contributing)
- [Security & Driver Signing](#security--driver-signing)
- [Troubleshooting](#troubleshooting)

## Features

- Presents a virtual capture endpoint device to Windows (a microphone named **MicyAudio**).
- Based on Microsoft `SimpleAudioSample` reference architecture adapted for virtual microphone use.
- Low-latency loopback of user-provided PCM audio into the Windows audio graph.
- Configurable sample rate / channels via driver settings (see `README` in `driver` folder).

## Why MicyWMD?

Virtual microphones are useful for:

- Injecting synthetic audio into conferencing apps (for automated tests or bots).
- Broadcasting system or app audio as a microphone input for streaming software.
- Research and experimentation with Windows audio stack and driver development.

This repository focuses on readability and education rather than enterprise production readiness.

## Prerequisites

- Windows 10 or 11 (x64) for development and testing.
- Visual Studio 2019 / 2022 with **Desktop development with C++** workload.
- Windows SDK (matching your target Windows version).
- Windows Driver Kit (WDK) installed (for building kernel/audio drivers and driver packages).
- `pnputil` (built into Windows) or `devcon` for driver installation during testing.

> For local development you will likely need **Test Signing** enabled (see Installing & Testing).

## Building

> These are general steps — adapt paths/names to the layout of this repo.

1. Clone the repository

```bash
git clone https://github.com/Mithronn/MicyWMD.git
cd MicyWMD
```

2. Open the Visual Studio solution `MicyAudio.sln` (or the `driver` project) and select the target platform (x64) and configuration (Debug or Release).

3. Build from Visual Studio or via the command line:

```powershell
# From Developer Command Prompt for VS
msbuild MicyAudio.sln /p:Configuration=Release /p:Platform=x64
```

4. If a driver package (.inf/.cat/.sys) is produced, it will be in `out\` or the project `bin` folder. Check the `driver` folder for packaging scripts.

## Installing & Testing

> **IMPORTANT (testing only):** Kernel-mode drivers on modern 64-bit Windows require proper signing to be loaded without disabling security. For development you can use Test Signing. Do **not** ask users to disable security on their machines for production use.

### Enable Test Signing (developer machine)

```powershell
# Run as Administrator
bcdedit /set testsigning on
# Reboot the machine
```

After you finish testing, disable test signing:

```powershell
bcdedit /set testsigning off
# Reboot again
```

### Install the driver package

Using `pnputil` (recommended):

```powershell
# Run as Administrator
pnputil /add-driver "path\to\MicyAudio.inf" /install
```

Using `devcon` (if you prefer):

```powershell
devcon install path\to\MicyAudio.inf ROOT\MicyAudio
```

After installation, open `Sound settings` or `Control Panel -> Sound` and check for an input device named **MicyAudio**. Select it as the default recording device for software that should receive injected audio.

## Usage

1. Start the user-space component (a player or audio source) that writes PCM audio into the driver ring buffer or provides the audio stream to the driver API.
2. In the target application (Zoom, Teams, OBS, Discord, etc.), select **MicyAudio** as the microphone/input device.
3. Play audio in the user-space app — it should appear as microphone input in the target app.

## Developer notes

- Key folders:
  - `Source/` — driver source, INF and packaging files
  - `Test/` — small user-space programs that feed audio to the driver
- Follow the code style already used in the repository; keep kernel-mode sections minimal and well-documented.
- When adding new features, include unit tests where feasible and a sample program demonstrating the feature.

## Contributing

Contributions are welcome! Please follow these guidelines:

1. Fork the repository.
2. Work on a feature branch (e.g. `feature/my-improvement`).
3. Submit a PR with a clear description and a linked issue if applicable.
4. Keep kernel-mode changes conservative and add detailed notes about why the change is safe.

## Security & Driver Signing

**Short version:** For production distribution of a kernel-mode virtual audio driver on 64-bit Windows, you must sign the driver and submit it to Microsoft for attestation/WHQL as required by the OS. Unsigned drivers or drivers with only self-signed certificates will generally not load on end-user machines without explicit changes to their boot configuration (not acceptable for general distribution).

**Recommendations:**

- For testing, enable `testsigning` on a development VM only.
- For distribution to end users, obtain an **EV Code Signing Certificate**, sign the driver package, and submit to Microsoft for attestation signing (or pursue WHQL if you need full certification). This makes the driver loadable on Windows 10/11 x64 without requiring users to modify security settings.
- Keep security surface minimal: validate all user-space inputs, avoid exposing arbitrary memory to untrusted processes, and sanitize parameters.

**Reporting vulnerabilities:** If you find a security issue, please report it privately to the maintainers (see `SECURITY.md` or open a private issue) — do not publish exploit details publicly.

## Troubleshooting

- Driver does not load:
  - Confirm you built for x64 and used the correct architecture in the INF.
  - For local testing, make sure `testsigning` is enabled or the driver is properly signed.
  - Check `Event Viewer` -> `System` for driver load/Service Control Manager errors.
- Device not visible in Sound settings:
  - Verify the INF installed correctly with `pnputil /enum-drivers`.
  - Confirm the driver registered the audio endpoint correctly.
- Audio distorted/latency issues:
  - Check sample rate and channel configuration between the audio source and driver.
  - Profile buffer sizes and scheduling in the sample client.
