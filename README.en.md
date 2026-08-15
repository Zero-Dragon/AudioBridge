<br>

<p align="center">
  <img src="assets/app-icon.png" width="150" height="150" alt="AudioBridge icon">
</p>

<h1 align="center">AudioBridge</h1>

<p align="center">
  <a href="README.md">简体中文</a> ·
  <strong>English</strong> ·
  <a href="README.ja.md">日本語</a>
</p>

AudioBridge is a Windows audio utility. It launches a selected music player or other application, captures its audio, and routes it directly to a DAC through an ASIO driver.

It is designed for applications without native ASIO output. Fake Output is enabled by default and silences the target application's normal WASAPI output, preventing duplicate playback through both the standard Windows output and the ASIO device.

## Preview

<p align="center">
  <img src="assets/app-preview.png" alt="AudioBridge application preview" width="960">
</p>

## Requirements

- Windows 10 version 1809 or later;
- .NET 10 Desktop Runtime;
- Windows App Runtime 1.8;
- a DAC manufacturer-provided ASIO driver, with the device recognized by Windows.

## Usage

1. Fully exit the target application.
2. Start AudioBridge and click Browse to select the application's `.exe`.
3. Select the DAC driver under ASIO output. AudioBridge keeps the driver's current clock source and shows the active source in Status.
4. Click Open, then start playback in the target application.
5. Confirm that a PID appears under Audio PIDs and the status reaches `Output Active`.

### Notes

- Do not use the same device for the Windows default audio output and ASIO output.
- Under Browse, select the main `.exe` that produces audio, not a launcher or updater.
- AudioBridge captures audio PIDs at application startup. Fully exit the target application, then open it through AudioBridge.
- `Extra prebuffer ms` is added on top of the application's WASAPI buffer. Status shows the negotiated `WASAPI Buffer`, and their sum determines the internal timeline capacity.
- Runtime logs are named by launch time and normally saved in the `logs` folder beside the application, with a local-app-data fallback when that folder is not writable. Each file is limited to about 4 MiB and retains the latest content.
- If there is no sound: when no Audio PID appears, exit every process belonging to the target application and reopen it through AudioBridge. When a PID is present, check the ASIO driver, sample rate, and any error or underrun shown in the app.

## Limitations

- Stereo audio only; no sample-rate conversion;
- applications using specialized audio interfaces or separate system services may not be captured;
- DLL injection may trigger security software warnings. Launch only applications you trust.

## Support the project

| Method | Support link |
| :---: | :---: |
| Ko-fi | [![Support AudioBridge on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/audiobridge)<br><br><img src="assets/kofi-qr.png" alt="AudioBridge Ko-fi support QR code" width="240"> |
| Alipay | <img src="assets/alipay-qr.png" alt="AudioBridge Alipay payment QR code" width="240"> |

## License

AudioBridge is licensed under [GNU GPL v3](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party licensing information.
