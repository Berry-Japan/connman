# 🌐 ConnMan for Berry Linux 🍓

ようこそ！これは **Berry Linux** 向けにカスタマイズされた **ConnMan** のリポジトリです！ 🚀
ConnManは、ネットワーク接続をシンプルかつ効率的に管理するための軽量なツールです。このフォークは、Berry Linuxの環境に最適化されています。💻

## ✨ 特徴

- **簡単なネットワーク管理** 🌍
  Wi-Fi、Ethernet、Bluetoothなど、さまざまな接続をスムーズに管理。
- **軽量で高速** ⚡
  Berry Linuxの軽快な哲学に合わせて、最小限のリソースで動作。
- **カスタマイズ性** 🛠️
  Berry Linuxユーザーのニーズに合わせた特別な設定と最適化。
- **オープンソース** 📖
  コミュニティによる貢献を歓迎！自由にコードをチェックして改良してください。

## 🛠️ インストール方法

1. **リポジトリをクローン** 📥
   ```bash
   git clone https://github.com/Berry-Japan/connman.git
   cd connman
   ```

2. **依存関係をインストール** 🧩
   Berry Linuxのパッケージマネージャを使って必要なパッケージを準備：
   ```bash
   sudo dnf install -y dbus-devel iptables-devel libmnl-devel readline-devel kernel-headers
   ```

3. **ビルドとインストール** 🔨
   ```bash
   ./configure --prefix=/usr
   make
   sudo make install
   ```

4. **ConnManを起動** 🚀
   ```bash
   sudo systemctl start connman
   sudo systemctl enable connman
   ```

## 📡 使用方法

- **ネットワークのスキャン** 🔍
  ```bash
  connmanctl scan wifi
  ```

- **Wi-Fiに接続** 📶
  ```bash
  connmanctl connect wifi_<network_id>
  ```

- **ステータス確認** 🔔
  ```bash
  connmanctl state
  ```

詳細な使い方は、[公式ConnManドキュメント](http://connman.net/)をチェック！📚

## 🐛 デバッグ

問題が発生した場合、以下の環境変数でデバッグ情報を取得できます：
- `CONNMAN_DHCP_DEBUG` 🕵️‍♂️ DHCP関連のデバッグ
- `CONNMAN_SUPPLICANT_DEBUG` 🔌 WPAサプリカントのデバッグ

例：
```bash
export CONNMAN_DHCP_DEBUG=1
connmanctl
```

## 🤝 コントリビューション

Berry Linuxコミュニティの一員として、ぜひ貢献してください！🙌
1. フォークして変更を加える
2. プルリクエストを送信
3. バグ報告や提案は[Issue](https://github.com/Berry-Japan/connman/issues)へ

## 📜 ライセンス

このプロジェクトは **GPL-2.0** ライセンスの下で公開されています。詳細は`LICENSE`ファイルを参照してください。📄

## 💬 連絡先

質問やフィードバックは、[Berry Japan GitHub](https://github.com/Berry-Japan) または [Issues](https://github.com/Berry-Japan/connman/issues) まで！📩

---

🌟 **Berry Linux** と **ConnMan** で、軽快なネットワーク体験を！ 🌟
