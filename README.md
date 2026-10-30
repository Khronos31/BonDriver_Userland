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

ライブラリと同じディレクトリに、同一のベース名で拡張子を `.ini` にした設定ファイルを配置します（例: `BonDriver_Siano.so` には `BonDriver_Siano.ini`）。`config/*.example` をコピーして作成してください。現在の実装はモジュールごとに 1 つの `[group.NAME]` を読みます。

INI 内で指定する相対パスは、**INI ファイルが置かれたディレクトリを基準**に解決されます。単純なコマンド名を指定した場合でも PATH 検索は行われず INI からの相対パスとして解決されるため、CLI のパスは絶対パスで指定してください。

このライブラリは、既に起動している daemon に `*-ts` クライアントを接続するだけです。daemon の起動・停止・監視は行いません。`*-ts` は shell を介さず argv 配列で起動し、`--instance`、`--receiver`、`--system`、`--freq`（Hz）、`--runtime-dir`、`--output -` を渡します。衛星は `--tsid`（または `--slot`）を渡します。`--frequency-khz` や `--stream-id` は使いません。LNB 15V はまだ設定できません（既定 0V）。

#### `[common]`

`[common]` は任意です。指定できるのは `lock_dir`、`tune_timeout_ms`、`stop_timeout_ms`、`kill_timeout_ms`、`buffer_limit_bytes`、`diagnostics` だけです。`lock_dir` はプロセス間排他用の書き込み可能なディレクトリです。

```ini
[common]
lock_dir = /run/bondriver-userland
tune_timeout_ms = 10000
stop_timeout_ms = 5000
kill_timeout_ms = 2000
buffer_limit_bytes = 8388608
diagnostics = stderr
```

#### `[client.NAME]`（実行ファイルと daemon runtime）

1 つの `*-ts` 実行ファイルと、その daemon の runtime ディレクトリを組にします。`NAME` は設定上のラベルで、製品種別の判定には使いません。`cli_path` と `runtime_dir` は必須です。

```ini
[client.px4]
cli_path = /usr/local/bin/px4-ts
runtime_dir = /run/px4-userland
```

`runtime_dir` は daemon が動いている間だけ実在すればよく、config 読み込み時に存在しなくても他の client の受信機は使えます。受信機を確保する時点で解決できなければ、その受信機を飛ばして診断ログを出します。

#### `[group.NAME]`（ホストに見せる受信機）

`tuner_name`、`channel_table`、`receivers` が必須です。`receivers` は `client:instance:receiver:systems` をセミコロンで区切ります。

```ini
[group.PX4]
tuner_name = PX4
channel_table = BonDriver_PX4.channels.tsv
receivers = px4:tok:2:T; px4:tok:3:T; px4:tok:0:S; px4:tok:1:S
```

- `systems` は `T`、`S`、`TS`。同じ放送方式を受信できる受信機だけを 1 つの group にまとめてください。EDCB では group 内の全 `Count` が同じチャンネル表を持つとみなされます。
- `instance` は 1..64 ASCII bytes、先頭は英数字、以降は英数字/`_`/`.`/`-`、末尾 `.` 不可、予約名（`con`/`prn`/`aux`/`nul`/`com1..9`/`lpt1..9`）不可です。px4/asicen/torne の現行 CLI は instance を小文字化するため、**小文字で指定してください**。
- 同じ物理受信機は、runtime ディレクトリの実体パス、instance、receiver の組で排他します。client ラベルや group 名を変えても二重確保されません。別製品が同じ runtime root と同じ instance 名を使うと、別の受信機でも排他されるため、製品ごとに runtime_dir を分けてください。
- 同名 section の再出現、未知 section、未知キー、未知 client 参照、重複する受信機、複数の `[group.*]` は設定エラーです。

### チャンネル定義ファイル (TSV)

`channel_table` で指定する外部チャンネル定義ファイルは、タブ区切りの TSV 形式です。各行に空間 ID、空間名、チャンネル ID、表示名、放送方式（`isdb-t` または `isdb-s`）、中心周波数（Hz）、ストリーム ID、スロット番号を記述します（不要な項目には `-` を指定）。

用途別の設定例を用意しています。環境に合わせてコピーして編集してください。

- 地上波: [config/channels-ground.example.tsv](config/channels-ground.example.tsv)
- BS / 110度CS: [config/channels-satellite.example.tsv](config/channels-satellite.example.tsv)
- CATV: [config/channels-catv.example.tsv](config/channels-catv.example.tsv)
- 地上波 + BS/CS: [config/channels-all.example.tsv](config/channels-all.example.tsv)

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
