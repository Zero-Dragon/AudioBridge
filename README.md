<br>

<p align="center">
  <img src="assets/app-icon.png" width="150" height="150" alt="AudioBridge 图标">
</p>

<h1 align="center">AudioBridge</h1>

<p align="center">
  <strong>简体中文</strong> ·
  <a href="README.en.md">English</a> ·
  <a href="README.ja.md">日本語</a>
</p>

AudioBridge 是一个 Windows 音频辅助工具。它可以从工具内启动音乐播放器或其他应用，捕获应用的声音，并通过 ASIO 驱动直接输出到 DAC。

它适合本身不支持 ASIO、但希望绕过普通 Windows 共享输出的应用。默认开启的 Fake Output 会将原来的 WASAPI 输出静音，避免声音同时从普通扬声器和 ASIO 设备播放。

## 运行预览

<p align="center">
  <img src="assets/app-preview.png" alt="AudioBridge 运行预览" width="960">
</p>

## 环境要求

- Windows 10 1809 或更高版本；
- .NET 10 Desktop Runtime；
- Windows App Runtime 1.8；
- 已安装 DAC 厂商提供的 ASIO 驱动，并确认设备可被 Windows 正常识别。

## 使用方法

1. 完全退出目标应用。
2. 启动 AudioBridge，点击 Browse 选择目标应用的 `.exe`。
3. 在 ASIO output 中选择 DAC 的 ASIO 驱动；驱动提供多个时钟源时，可在 ASIO clock source 中选择。
4. 点击 Open，再在目标应用中开始播放音频。
5. 确认 Audio PIDs 中出现 PID，状态变为 `Output Active`。

### 注意

- Windows 默认音频输出与 ASIO output 不要使用同一台设备；
- Browse 请选择实际播放音频的主程序 `.exe`，不要选择 launcher 或 updater；
- AudioBridge 需要在应用启动时捕获音频 PID，请先完全退出目标应用，再通过 AudioBridge 打开；
- 如果没有声音：没有 Audio PID 时，请退出目标应用的所有进程并重新打开；已经有 PID 时，请检查 ASIO 驱动、采样率和界面中的错误或 underrun 信息。

## 使用限制

- 当前只支持立体声音频，不进行采样率转换；
- 少数使用特殊音频接口或独立系统服务的应用可能无法捕获；
- DLL 注入可能触发安全软件警告，请只打开你信任的应用。

## 支持项目

| 方式 | 支持入口 |
| :---: | :---: |
| Ko-fi | [![Support AudioBridge on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/audiobridge)<br><br><img src="assets/kofi-qr.png" alt="AudioBridge Ko-fi 支持二维码" width="240"> |
| 支付宝 | <img src="assets/alipay-qr.png" alt="AudioBridge 支付宝收款二维码" width="240"> |

## 许可证

AudioBridge 采用 [GNU GPL v3](LICENSE) 许可证。第三方组件的许可信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
