# BonDriver_Userland

`BonDriver_Userland` は、Siano・PX4・ASICEN・PIX・torne の userland CLI（`*-ts`）をバックエンドとして起動し、標準出力から得られる MPEG-TS を EDCB や TVTest、LibISDB などの BonDriver 対応アプリケーションへ渡す共有ライブラリです。配布する本体は 1 種類で、必要なホストは同じバイナリを別名でコピーして使います。

各 userland CLI、ファームウェア、daemon の導入や設定手順は、それぞれのプロジェクトを参照してください。

## 対応環境 / 必要なもの

各環境向けに以下の共有ライブラリを提供します。

| Linux x64 / arm64 | Windows x64 | macOS arm64 |
| --- | --- | --- |
| `BonDriver_Userland.so` | `BonDriver_Userland.dll` | `BonDriver_Userland.dylib` |

### 動作要件

- **Linux (x64 / arm64)**: glibc 2.28 以上、または musl 1.2.5 以上（libc ごとに個別バイナリ）
- **Windows (x64)**: 64-bit Windows 環境
- **macOS (arm64)**: macOS 11 以降（ビルド最低対象。実際の動作確認は macOS 26 で実施）

> [!NOTE]
> 実機、EDCB/TVTest/tsdump への実組み込み、Windows/macOS/musl の実動作は未検証です。Linux x86_64 の fake/実CLI parse 検証のみ実施しています。

## 導入と最短設定

### バイナリの入手

タグリリース時に、以下の 6 構成のアーカイブを [Releases](https://github.com/Khronos31/BonDriver_Userland/releases) で公開する予定です（現在はリリース未作成）。各アーカイブに `BonDriver_Userland` 本体、代表設定例、チャンネル定義例、各種ドキュメントが含まれます。

- Linux glibc (x64 / arm64)
- Linux musl (x64 / arm64)
- Windows x64
- macOS arm64

### アプリケーションへの配置

EDCB や TVTest などのアプリケーションで利用する場合は、各アプリの指定する BonDriver 読み込み場所（`BonDriver/` フォルダなど）へライブラリ本体（`.dll` や `.so`）と INI ファイルを配置してください。なお、macOS で `.dylib` を読み込めるかどうかは、利用するアプリケーション側の対応に依存します。

### INI ファイルの配置

ライブラリと同じディレクトリに `BonDriver_Userland.ini` を配置します。すべての `BonDriver_Userland*` コピーがこの 1 ファイルを参照します。`config/BonDriver_Userland.ini.example` をコピーして作成してください。

モジュールの basename が `[group.NAME]` を選びます。`BonDriver_Userland.so` は `[group.BonDriver_Userland]`、`BonDriver_Userland_T.so` は `[group.BonDriver_Userland_T]` です。末尾の `_<数字>` は無視するため、`BonDriver_Userland_T_0.so` も `[group.BonDriver_Userland_T]` を使います。一致する group が無い場合は暗黙の既定へ落とさず、利用可能な group 名を添えて設定エラーになります。

INI 内で指定する相対パスは、**INI ファイルが置かれたディレクトリを基準**に解決されます。単純なコマンド名を指定した場合でも PATH 検索は行われず INI からの相対パスとして解決されるため、CLI のパスは絶対パスで指定してください。

このライブラリは、既に起動している daemon に `*-ts` クライアントを接続するだけです。daemon の起動・停止・監視は行いません。`*-ts` は shell を介さず argv 配列で起動し、`--instance`、`--receiver`、`--system`、`--freq`（Hz）、`--runtime-dir`、`--output -` を渡します。衛星は `--tsid`（または `--slot`）を渡します。`--frequency-khz` や `--stream-id` は使いません。LNB 15V はまだ設定できません（既定 0V）。

#### `[common]`

`[common]` は任意です。指定できるのは `lock_dir`、`tune_timeout_ms`、`stop_timeout_ms`、`kill_timeout_ms`、`buffer_limit_bytes`、`diagnostics` だけです。`lock_dir` はプロセス間排他用の書き込み可能なディレクトリです。**同じ受信機を参照するすべての BonDriver コピー/INI で同じ `lock_dir` を共有してください。** 省略時は全 INI で同じ既定値になりますが、INI ごとに別の `lock_dir` を指定すると、BonDriver 側の事前排他は共有されません（受信機の競合は daemon の busy 応答まで検出が遅れます）。

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

`tuner_name`、`channel_table`、`receivers` が必須です。`receivers` は `client:instance:receiver:systems` をセミコロンで区切ります。1 つの INI に複数の group と複数の client を書けます。

```ini
[group.BonDriver_Userland_T]
tuner_name = Userland-T
channel_table = channels-ground.example.tsv
receivers = px4:q3u4:2:T; px4:q3u4:3:T; siano:s1ud_a:0:T; torne:torne0:0:T

[group.BonDriver_Userland_S]
tuner_name = Userland-S
channel_table = channels-satellite.example.tsv
receivers = px4:q3u4:0:S; px4:q3u4:1:S
```

- `systems` は `T`、`S`、`TS`。同じ放送方式を受信できる受信機だけを 1 つの group にまとめてください。EDCB では group 内の全 `Count` が同じチャンネル表を持つとみなされます。
- `instance` は 1..64 ASCII bytes、先頭は英数字、以降は英数字/`_`/`.`/`-`、末尾 `.` 不可、予約名（`con`/`prn`/`aux`/`nul`/`com1..9`/`lpt1..9`）不可です。特定版の source では px4 と pix の product CLI が instance を小文字化し、asicen と torne は保持します。上流で修正中のため恒久契約とは扱わず、実 daemon での再現も未実施です。**例は小文字で統一しています。**
- 同じ物理受信機は、runtime ディレクトリの実体パス、instance、receiver の組で排他します。client ラベルや group 名を変えても二重確保されません。別製品が同じ runtime root と同じ instance 名を使うと、別の受信機でも排他されるため、製品ごとに runtime_dir を分けてください。この排他は `lock_dir` が同じ場合に限ります。
- 同名 section の再出現、未知 section、未知キー、未知 client 参照、重複する受信機は設定エラーです。
- EDCB では放送方式ごとに BonDriver を分けてください。同じバイナリを `BonDriver_Userland_T` / `_S` / `_TS` へコピーすれば、各コピーが同じ INI の対応 group を読みます。TVTest/tsdump は全受信機をまとめた `[group.BonDriver_Userland]` を使えます。
- 代表例は T12 / S8 / TS3 = 23 受信機です（S1UD 2 台、Q3U4/M1UR/S1UR/W3U3/W3U2/DT181/DT295 各 1 台、torne CECH-ZD1J 1 台）。torne は `torne-ts` receiver 0 / ISDB-T のみの予定で、daemon listener と実受信はまだ成立していません。全 23 同時動作や S1UD 2 台同時は検証済みではありません。

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
