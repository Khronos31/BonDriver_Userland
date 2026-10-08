# BonDriver_Userland

`BonDriver_Siano` および `BonDriver_PX4` は、[siano-userland](https://github.com/Khronos31/siano-userland) や [px4-userland](https://github.com/Khronos31/px4-userland) の CLI ツールをバックエンドとして起動し、標準出力から得られる MPEG-TS を EDCB や TVTest、LibISDB などの BonDriver 対応アプリケーションへ渡す共有ライブラリです。

- **BonDriver_Siano**: Siano 系チューナーによる地上波受信に対応
- **BonDriver_PX4**: PX4 系チューナーによる地上波および衛星（BS/CS）受信に対応

バックエンドとして動作させるための各 userland CLI（`siano-ts` / `px4-ts`）、ファームウェア、daemon（`px4d`）の導入や設定手順は、それぞれのプロジェクトを参照してください。

## 対応環境 / 必要なもの

各環境向けに以下の共有ライブラリを提供します。

| バックエンド | Linux x64 / arm64 | Windows x64 | macOS arm64 |
| --- | --- | --- | --- |
| Siano | `BonDriver_Siano.so` | `BonDriver_Siano.dll` | `BonDriver_Siano.dylib` |
| PX4 | `BonDriver_PX4.so` | `BonDriver_PX4.dll` | `BonDriver_PX4.dylib` |

### 動作要件

- **Linux (x64 / arm64)**: glibc 2.28 以上、または musl 1.2.5 以上（libc ごとに個別バイナリ）
- **Windows (x64)**: 64-bit Windows 環境
- **macOS (arm64)**: macOS 11 以降（ビルド最低対象。実際の動作確認は macOS 26 で実施）

> [!NOTE]
> Windows 版 PX4 は、将来 Windows 向けに同等オプションの `px4-ts` CLI が提供されることを見越した先行対応です。実機や各種再生・録画アプリケーションを用いた受入検証は未実施です。

## 導入と最短設定

### バイナリの入手

タグリリース時に、以下の 6 構成のアーカイブを [Releases](https://github.com/Khronos31/BonDriver_Userland/releases) で公開する予定です（現在はリリース未作成）。各アーカイブに両 BonDriver ライブラリ本体、設定例、チャンネル定義例、各種ドキュメントが含まれます。

- Linux glibc (x64 / arm64)
- Linux musl (x64 / arm64)
- Windows x64
- macOS arm64

### アプリケーションへの配置

EDCB や TVTest などのアプリケーションで利用する場合は、各アプリの指定する BonDriver 読み込み場所（`BonDriver/` フォルダなど）へライブラリ本体（`.dll` や `.so`）と INI ファイルを配置してください。なお、macOS で `.dylib` を読み込めるかどうかは、利用するアプリケーション側の対応に依存します。

### INI ファイルの配置

ライブラリと同じディレクトリに、同一のベース名で拡張子を `.ini` にした設定ファイルを配置します（例: `BonDriver_Siano.so` には `BonDriver_Siano.ini`）。`config/*.example` をコピーして作成してください。

INI 内で指定する相対パスは、**INI ファイルが置かれたディレクトリを基準**に解決されます。単純なコマンド名を指定した場合でも PATH 検索は行われず INI からの相対パスとして解決されるため、CLI のパスは絶対パスで指定してください。

#### 最低限必要な共通設定 (`[common]`)

`[common]` セクションでは `tuner_name`、`cli_path`、`channel_table` の 3 つが必須項目です。

```ini
[common]
; アプリケーションへ通知するチューナー名（必須）
tuner_name = Siano

; 起動するバックエンド CLI の絶対パス（必須、環境に合わせて書き換えてください）
cli_path = /usr/local/bin/siano-ts

; 使用するチャンネル定義ファイルのパス（必須、INI からの相対パスまたは絶対パス）
channel_table = BonDriver_Siano.channels.tsv

; （任意）プロセス間排他用ディレクトリ。書き込み可能な場所を指定
lock_dir = /run/bondriver-userland
```

## 受信機とチャンネル設定の使い方

### バックエンド別の設定

#### Siano (`[siano]`)

特定のチューナーを固定利用する場合は `device`、複数台から空きを自動確保する場合は `devices`（カンマ区切り）のどちらかを指定します。

```ini
[siano]
; チューナーのセレクター番号（単一指定）
device = 0

; 任意: ファームウェアのパス
firmware = /lib/firmware/isdbt_rio.inp
```

#### PX4 (`[px4]`)

あらかじめ起動している `px4d` のランタイムディレクトリを指定します。

```ini
[px4]
; 稼働中 px4d のランタイムディレクトリ（実在するパスが必須）
runtime_dir = /run/px4-userland

; 単一受信機を指定する場合（instance, receiver, systems）
instance = tok
receiver = 0
systems = TS

; 複数受信機をプール利用する場合（receivers でセミコロン区切り列挙）
; receivers = tok:0:T;tok:1:S;tok:2:TS

; LNB 給電（15V）を行う場合の設定（px4d が --allow-lnb-power で稼働している必要があります）
lnb_voltage = 0
allow_lnb_15 = 0
```

### チャンネル定義ファイル (TSV)

`channel_table` で指定する外部チャンネル定義ファイルは、タブ区切りの TSV 形式です。各行に空間 ID、空間名、チャンネル ID、表示名、放送方式（`isdb-t` または `isdb-s`）、中心周波数（Hz）、ストリーム ID、スロット番号を記述します（不要な項目には `-` を指定）。

用途別の設定例を用意しています。環境に合わせてコピーして編集してください。

- 地上波: [config/channels-ground.example.tsv](config/channels-ground.example.tsv)
- BS / 110度CS: [config/channels-satellite.example.tsv](config/channels-satellite.example.tsv)
- CATV: [config/channels-catv.example.tsv](config/channels-catv.example.tsv)

## ソースビルド

### 必要なツール

- C++17 対応コンパイラ
- CMake 3.16 以上

### ビルド手順

```sh
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
```

静的ランタイムリンクなどのリリース向け詳細オプションについては、[docs/release-build.md](docs/release-build.md) を参照してください。

> [!NOTE]
> FreeBSD や Android (Termux) 環境もソースビルドの対象ですが、動作確認は行っていません。Termux で利用する場合は Android API 28 以上を対象とし、`lock_dir` に書き込み権限のある専用ディレクトリを指定してください。

## MITライセンス

本プロジェクトは MIT License のもとで公開されます。詳細は [LICENSE](LICENSE) および [NOTICE](NOTICE) を参照してください。
