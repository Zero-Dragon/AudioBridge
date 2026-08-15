<br>

<p align="center">
  <img src="assets/app-icon.png" width="150" height="150" alt="Tick By Tick アイコン">
</p>

<h1 align="center">Tick By Tick</h1>

<p align="center">
  <a href="README.md">简体中文</a> ·
  <a href="README.en.md">English</a> ·
  <strong>日本語</strong>
</p>

Tick By Tick は Windows 向けのオーディオユーティリティです。選択した音楽プレーヤーなどのアプリを起動し、その音声をキャプチャして ASIO ドライバー経由で DAC へ直接出力します。

ASIO 出力に対応していないアプリでの利用を想定しています。既定で有効な Fake Output は、対象アプリの通常の WASAPI 出力を無音化し、Windows の通常出力と ASIO 機器の両方から重複して再生されることを防ぎます。

## 動作プレビュー

<p align="center">
  <img src="assets/app-preview.png" alt="Tick By Tick の動作プレビュー" width="960">
</p>

## 動作要件

- Windows 10 バージョン 1809 以降
- .NET 10 Desktop Runtime
- Windows App Runtime 1.8
- DAC メーカーが提供する ASIO ドライバーがインストールされ、Windows から機器を正常に認識できること。

## 使い方

1. 対象アプリを完全に終了します。
2. Tick By Tick を起動し、Browse で対象アプリの `.exe` を選択します。
3. ASIO output で DAC の ASIO ドライバーを選択します。Tick By Tick はドライバーの現在のクロックソースを維持し、使用中のソースを Status に表示します。
4. Open をクリックし、対象アプリで音声を再生します。
5. Audio PIDs に PID が表示され、状態が `Output Active` になることを確認します。

### 注意

- Windows の既定の音声出力と ASIO output に同じ機器を使用しないでください。
- Browse では、音声を出力するメインの `.exe` を選択し、launcher や updater は選択しないでください。
- Tick By Tick はアプリの起動時に音声 PID を取得します。対象アプリを完全に終了してから、Tick By Tick 経由で開いてください。
- `Extra prebuffer ms` はアプリの WASAPI buffer に追加されるバッファーです。Status には実際の `WASAPI Buffer` が表示され、その合計を基準に内部タイムライン容量を計算します。
- 実行ログは起動時刻ごとの名前で、通常はアプリと同じ場所の `logs` フォルダーに保存されます。書き込めない場合はローカルアプリデータへ保存されます。各ファイルは約 4 MiB に制限され、超過後は最新の内容を保持します。
- 音が出ない場合は、Audio PID が表示されていなければ対象アプリの全プロセスを終了し、Tick By Tick 経由で開き直してください。PID が表示されている場合は、ASIO ドライバー、サンプルレート、および画面上のエラーや underrun を確認してください。

## 制限事項

- ステレオ音声のみをサポートし、サンプルレート変換は行いません。
- 特殊なオーディオインターフェイスや独立したシステムサービスを使用するアプリは、キャプチャできない場合があります。
- DLL 注入がセキュリティソフトの警告対象になる場合があります。信頼できるアプリのみ起動してください。

## プロジェクトを支援する

| 方法 | 支援先 |
| :---: | :---: |
| Ko-fi | [![Support Tick By Tick on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/audiobridge)<br><br><img src="assets/kofi-qr.png" alt="Tick By Tick Ko-fi 支援用 QR コード" width="240"> |
| Alipay | <img src="assets/alipay-qr.png" alt="Tick By Tick Alipay 支払い用 QR コード" width="240"> |

## ライセンス

Tick By Tick は [GNU GPL v3](LICENSE) で提供されます。第三者コンポーネントのライセンス情報は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
