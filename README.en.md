<br>

<p align="center">
  <img src="assets/app-icon.png" width="150" height="150" alt="Tick By Tick icon">
</p>

<h1 align="center">Tick By Tick</h1>

<p align="center">
  <a href="README.md">简体中文</a> ·
  <strong>English</strong> ·
  <a href="README.ja.md">日本語</a>
</p>

Tick By Tick is a Windows audio utility. It launches a selected music player or other application, captures its audio, and routes it directly to a DAC through an ASIO driver.

It is designed for applications without native ASIO output.

## Audio path

- **Automatic sample-rate matching**: Matches the DAC sample rate when the input PCM format changes;
- **DAC-clock-driven flow**: Advances audio data according to actual DAC consumption;
- **Real-time ASIO output**: Processes audio data through ASIO with low overhead and high priority;
- **Adjustable prebuffer**: Absorbs clock jitter caused by brief data-delivery gaps and thread scheduling.

## Preview

<p align="center">
  <img src="assets/app-preview.png" alt="Tick By Tick application preview" width="960">
</p>

## Requirements

- Windows 10 version 1809 or later;
- .NET 10 Desktop Runtime;
- Windows App Runtime 1.8;
- a DAC manufacturer-provided ASIO driver, with the device recognized by Windows.

## Usage

1. Fully exit the target application.
2. Start Tick By Tick and click Browse to select the application's `.exe`.
3. Select the DAC driver under ASIO output.
4. Click Open, then start playback in the target application.
5. Confirm that a PID appears under Audio PIDs and the status reaches `Output Active`.

### Notes

- Do not use the same device for the Windows default audio output and ASIO output.
- Select the main `.exe` that produces audio, not a launcher or updater.
- Audio PIDs must be captured when the application starts. Fully exit the target application, then open it through Tick By Tick.
- Fake Output is enabled by default and silences the target application's normal WASAPI output, preventing duplicate playback through both the standard Windows output and the ASIO device.
- If there is no sound: when no Audio PID appears, exit every process belonging to the target application and reopen it through Tick By Tick. When a PID is present, check the ASIO driver, sample rate, and any error or underrun shown in the app.

## Limitations

- Stereo audio only; no sample-rate conversion;
- applications using specialized audio interfaces or separate system services may not be captured;
- DLL injection may trigger security software warnings. Launch only applications you trust.

## Support the project

| Method | Support link |
| :---: | :---: |
| Ko-fi | [![Support Tick By Tick on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/audiobridge)<br><br><img src="assets/kofi-qr.png" alt="Tick By Tick Ko-fi support QR code" width="240"> |
| Alipay | <img src="assets/alipay-qr.png" alt="Tick By Tick Alipay payment QR code" width="240"> |

## License

Tick By Tick is licensed under [GNU GPL v3](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party licensing information.
