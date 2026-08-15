<br>

<p align="center">
  <img src="assets/app-icon.png" width="150" height="150" alt="Tick By Tick 圖示">
</p>

<h1 align="center">Tick By Tick</h1>

<p align="center">
  <a href="README.md">简体中文</a> ·
  <strong>繁體中文</strong> ·
  <a href="README.en.md">English</a> ·
  <a href="README.ja.md">日本語</a>
</p>

Tick By Tick 是一款 Windows 音訊輔助工具。它可以從工具內啟動音樂播放器或其他應用程式，擷取應用程式的聲音，並透過 ASIO 驅動程式直接輸出至 DAC。

它適合本身不支援 ASIO、但希望繞過一般 Windows 共用輸出的應用程式。

軟體內部針對音訊時鐘穩定性做了專門設計，歡迎追求高音質的發燒友體驗～

如果 Tick By Tick 對你有所幫助，[歡迎請我喝杯咖啡 ☕](#支援專案)

## 音訊鏈路

- **採樣率自動跟隨**：輸入 PCM 格式變更時自動配對 DAC 採樣率
- **DAC 時鐘驅動**：依照 DAC 的實際消耗推動音訊資料流動
- **即時 ASIO 輸出**：ASIO 以低負載、高優先級擷取資料
- **可調預先緩衝**：吸收短時間資料提交與執行緒排程產生的時鐘抖動

## 執行預覽

<p align="center">
  <img src="assets/app-preview.png" alt="Tick By Tick 執行預覽" width="960">
</p>

## 環境需求

- Windows 10 1809 或更高版本；
- .NET 10 Desktop Runtime；
- Windows App Runtime 1.8；
- 已安裝 DAC 製造商提供的 ASIO 驅動程式，並確認 Windows 可正常辨識裝置。

## 使用方法

1. 完全結束目標應用程式。
2. 啟動 Tick By Tick，按一下 Browse 選擇目標應用程式的 `.exe`。
3. 在 ASIO output 中選擇 DAC 的 ASIO 驅動程式；
4. 按一下 Open，再於目標應用程式中開始播放音訊。
5. 確認 Audio PIDs 中出現 PID，且狀態變為 `Output Active`。

### 注意

- Windows 預設音訊輸出與 ASIO output 請勿使用同一台裝置；
- 請選擇實際播放音訊的主程式 `.exe`，不要選擇 launcher 或 updater；
- 需要在應用程式啟動時擷取音訊 PID，請先完全結束目標應用程式，再透過 Tick By Tick 開啟；
- 如果沒有聲音：未出現 Audio PID 時，請結束目標應用程式的所有處理程序後重新開啟；已有 PID 時，請檢查 ASIO 驅動程式、採樣率，以及介面中的錯誤或 underrun 資訊。

### 設定說明

| 參數 | 說明 |
| --- | --- |
| **Extra prebuffer ms** | 在應用程式自身的 WASAPI 緩衝之外，額外保留的音訊時長。增大可提高對短時間資料提交中斷和執行緒抖動的容忍度，但也會增加播放延遲。 |
| **Max buffer advance ms** | 控制低水位補靜音的觸發位置。當受保護的音訊餘量低於 `Extra prebuffer ms - Max buffer advance ms` 時，Tick By Tick 會補充橋接靜音以維持 ASIO 連續執行。 |
| **ASIO buffer frames** | 指定 ASIO 驅動程式每頁緩衝區的影格數。`0` 表示使用驅動程式的偏好值。 |
| **Fake Output** | 使用虛擬 WASAPI 輸出擷取目標應用程式，並防止聲音同時輸出至 Windows 預設裝置。 |

## 使用限制

- 目前僅支援立體聲音訊，不進行採樣率轉換；ASIO 驅動程式必須原生支援音源採樣率；
- 少數使用特殊音訊介面或獨立系統服務的應用程式可能無法擷取；
- DLL 注入可能觸發安全軟體警告，請只開啟你信任的應用程式。

## 支援專案

| 方式 | 支援入口 |
| :---: | :---: |
| Ko-fi | [![Support Tick By Tick on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/audiobridge)<br><br><img src="assets/kofi-qr.png" alt="Tick By Tick Ko-fi 支援 QR Code" width="240"> |
| 支付寶 | <img src="assets/alipay-qr.png" alt="Tick By Tick 支付寶收款 QR Code" width="240"> |

## 授權條款

Tick By Tick 採用 [GNU GPL v3](LICENSE) 授權條款。第三方元件的授權資訊請參閱 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
