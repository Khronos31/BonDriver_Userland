# BonDriver_Userland

`BonDriver_Userland` は、Siano・PX4・ASICEN・PIX・torne などの userland CLI（`*-ts`）をバックエンドとして起動し、標準出力から取得した MPEG-TS を EDCB や TVTest、tsdump などの BonDriver 対応アプリケーションへ渡す共有ライブラリである。

配布するライブラリ本体は全製品共通の 1 種類であり、ホスト側の構成に合わせて同一バイナリを実体コピーして使用する。

各 userland CLI、ファームウェア、および daemon の導入や設定手順は、それぞれのプロジェクトのドキュメントを参照すること。

## 動作の仕組みと運用前提

本ライブラリは、**あらかじめ起動している各 userland daemon** に対してクライアント CLI（`*-ts`）を子プロセスとして起動し、標準出力から raw TS を受け取る。

- **daemon の管理**: daemon の起動・停止・死活監視・サービス登録は本ライブラリの管轄外である。OS サービス（systemd 等）、Home Assistant のアドオン、Pochitto、あるいは手動起動など、環境側で事前に daemon を起動・管理すること。
- **リソースの解放**: アプリケーションから `Close` が呼ばれると、本ライブラリが起動した子プロセス（`*-ts`）を停止し、確保していた受信機リースのみを解放する。daemon 自体は常駐し続ける。
- **子プロセスの起動**: `*-ts` はシェルを介さず argv 配列で直接起動する。`--instance`、`--receiver`、`--system`、`--freq`（Hz）、`--runtime-dir`、`--output -` を渡し、衛星放送では `--tsid`（または `--slot`）を渡す（LNB 給電等の機器設定は各 daemon 側で管理する）。

## 対応環境と配布パッケージ

### 配布パッケージ（6 構成）

タグ付きリリース時に、以下の 6 構成のアーカイブを [Releases](https://github.com/Khronos31/BonDriver_Userland/releases) で配布する。

| 対象プラットフォーム | アーカイブ形式 | 共有ライブラリ名 | 動作要件 / 備考 |
| --- | --- | --- | --- |
| **Windows x64** | `.zip` | `BonDriver_Userland.dll` | 64-bit Windows、MSVC 静的ランタイム（/MT）リンク |
| **Linux x86_64 (glibc)** | `.tar.gz` | `BonDriver_Userland.so` | glibc 2.28 以上 |
| **Linux x86_64 (musl)** | `.tar.gz` | `BonDriver_Userland.so` | musl 1.2.5 以上 |
| **Linux arm64 (glibc)** | `.tar.gz` | `BonDriver_Userland.so` | glibc 2.28 以上 |
| **Linux arm64 (musl)** | `.tar.gz` | `BonDriver_Userland.so` | musl 1.2.5 以上 |
| **macOS arm64** | `.tar.gz` | `BonDriver_Userland.dylib` | macOS 11.0 以降（Apple Silicon） |

各アーカイブには、ライブラリ本体に加えて代表設定例（`BonDriver_Userland.ini.example`）、チャンネル定義例（`channels-*.example.tsv`）、ビルド情報（`BUILD-INFO.txt`）、チェックサム（`SHA256SUMS`）、ドキュメント類が同梱される。

### 対象アプリケーション

- **主要対応対象**: EDCB、TVTest、tsdump
- **互換確認候補**: BonDriverProxyEx、BonDriverProxy_Linux

## アプリケーションへの導入と配置

### 1 バイナリ・1 INI・実体コピーの原則

BonDriver の仕様上、1 つのモジュールオブジェクトが同時に制御できるのは「1 チャンネル / 1 TS」である。そのため、DLL ファイル数と物理チューナー数は必ずしも 1:1 にはならない。

ホストアプリケーションで複数のチューナーや異なる放送方式を使い分ける場合、配布される同一の共有ライブラリを必要な数だけ**別名で実体コピー**して配置する（シンボリックリンクやハードリンクではなく、別ファイルとしてコピーすること）。

- **EDCB での配置**:
  EDCB は 1 つの BonDriver モジュールに属する全チューナー（Count）が同一のチャンネル定義（ChSet4）を持つ前提で動作する。そのため、放送方式ごとにモジュールを分ける必要がある。同一バイナリを以下のように実体コピーして配置する。
  - `BonDriver_Userland_T.dll`（地上波専用）
  - `BonDriver_Userland_S.dll`（BS/110度CS専用）
  - `BonDriver_Userland_TS.dll`（地上波/衛星兼用）
  ※ EDCB のチューナー数（Count）は `EpgTimerSrv.ini` 側で設定する。
- **TVTest / tsdump での配置**:
  全チューナーをまとめた単一のモジュール `BonDriver_Userland.dll`（または `.so`）を配置して利用できる。利用者が画面やコマンドライン引数で受信チャンネルを選択する。
- **BonDriverProxyEx / BonDriverProxy_Linux での配置**:
  プロキシ配下で同時起動する独立インスタンス数に応じて、`BonDriver_Userland_T_0.dll`、`BonDriver_Userland_T_1.dll` などのように実体コピーを用意する。

### INI ファイルの配置と group 解決

ライブラリ本体と同じディレクトリに `BonDriver_Userland.ini` を 1 つ配置する。すべてのコピー（`BonDriver_Userland*.dll` 等）がこの同一ファイルを読み込む。

1. モジュールのファイル名（拡張子を除いた basename）に基づき、INI 内の `[group.<basename>]` を選択する。
   - 例: `BonDriver_Userland.dll` → `[group.BonDriver_Userland]`
   - 例: `BonDriver_Userland_T.dll` → `[group.BonDriver_Userland_T]`
2. 一致するセクションが見つからない場合、ファイル名末尾の `_<数字>` を 1 段階ずつ順に除去して再検索する（例: `_T_0_1` → `_T_0` → `_T`）。
   - 例: `BonDriver_Userland_T_0.dll` → `[group.BonDriver_Userland_T]`
3. それでも一致するセクションがない場合は、利用可能な group 名をログに残して設定エラーとなる（暗黙の既定値へフォールバックすることはない）。

## 設定ファイル（BonDriver_Userland.ini）の仕様

`config/BonDriver_Userland.ini.example` をライブラリ本体と同じディレクトリへコピーし、`BonDriver_Userland.ini` にリネームして編集する。

INI ファイル内で指定する相対パスは、**INI ファイルが置かれたディレクトリを基準**に解決される。CLI 実行ファイルのパスには絶対パスの指定を推奨する。

### セクション構成

#### 1. `[common]`（共通設定・任意）

プロセス間排他や各種タイムアウトを設定する。

```ini
[common]
lock_dir = /run/bondriver-userland
tune_timeout_ms = 10000
stop_timeout_ms = 5000
kill_timeout_ms = 2000
buffer_limit_bytes = 8388608
diagnostics = stderr
```

> [!IMPORTANT]
> **同一の受信機を参照するすべての BonDriver コピーおよび INI ファイルで、必ず同じ `lock_dir` を指定すること。**
> `lock_dir` が異なるとプロセス間排他ファイルが別々に作成され、BonDriver 側の事前排他が機能しなくなる（競合の検出が daemon 側の busy 応答まで遅れる）。省略時はプラットフォームごとの既定共通パスが使われる。

#### 2. `[client.<NAME>]`（CLI と runtime の定義）

`*-ts` 実行ファイルと、対応する daemon の runtime ディレクトリを組にして定義する。`<NAME>` は設定内の識別ラベルであり、製品種別の判定には使わない。

```ini
[client.px4]
cli_path = /usr/local/bin/px4-ts
runtime_dir = /run/px4-userland
```

- `cli_path`: `*-ts` 実行ファイルのパス（必須）。
- `runtime_dir`: 各 daemon がソケットや一時ファイルを置くディレクトリ（必須）。
- ※ `runtime_dir` の実在確認は受信機を確保する時点で実施される。ある client の daemon が停止していても、設定読み込み時にエラーとはならず、稼働している他 client の受信機を利用できる。

#### 3. `[group.<NAME>]`（ホストへ公開するチューナー群）

ホストアプリケーションに見せるチューナー群を定義する。モジュールの basename に対応する group が読み込まれる。

```ini
[group.BonDriver_Userland_T]
tuner_name = Userland-T
channel_table = channels-ground.example.tsv
receivers = siano:s1ud_a:0:T; siano:s1ud_b:0:T; px4:q3u4:2:T; px4:q3u4:3:T
```

- `tuner_name`: ホストに通知するチューナー名（必須）。
- `channel_table`: 外部チャンネル定義 TSV ファイルへのパス（必須）。
- `receivers`: セミコロン（`;`）区切りで受信機を列挙する（必須、1 行で記述）。
  - 書式: `client:instance:receiver:systems`
    - `client`: `[client.<NAME>]` で定義したラベル名。
    - `instance`: 各 daemon で設定したインスタンス名。1〜64 文字の ASCII 英数字、`_`、`.`、`-` が使用可能（先頭は英数字、末尾 `.` 不可、予約名不可）。**小文字での指定を推奨する。**
    - `receiver`: チューナー番号（0〜7）。
    - `systems`: 受信可能な放送方式（`T`、`S`、`TS`）。
- **方式の整合性**: EDCB では 1 つの group 内の全チューナーが同一のチャンネル表を処理できる必要がある。地上波専用機（`T`）と衛星専用機（`S`）を同一の group に混在させると、EDCB の自動選局で受信不能なチューナーへ割り当てられる原因となるため、方式ごとに group を分離すること。

### 排他と設定検査のルール

- **物理受信機の排他**: 物理的な受信機は `(正規化された runtime_dir, instance, receiver)` の組み合わせで一意に識別・排他される。client ラベルや group 名が異なっていても、同一の物理受信機が二重に確保されることはない（同一の `lock_dir` を共有している場合に限る）。
- **設定構文の厳格検査**: 同一セクション名の再出現、未知のセクション、未知のキー、未定義の client 参照、同一 group 内での受信機の重複定義は設定エラーとなる。

## 代表設定例（23 受信機構成）

同梱の `config/BonDriver_Userland.ini.example` は、以下の手持ち実機（計 23 チューナー）を想定した構成例である。

- PX-S1UD 2 台（地デジ専用各 1）
- PX-Q3U4 1 台（地デジ 4 / BS・CS 4）
- PX-S1UR 1 台（地デジ専用 1）
- PX-M1UR 1 台（地デジ/BS/CS 兼用 1）
- PX-W3U3 1 台（地デジ 2 / BS・CS 2）
- PX-W3U2 1 台（地デジ 2 / BS・CS 2）
- PIX-DT181-PU0 1 台（地デジ/BS/CS 兼用 1）
- PIX-DT295 1 台（地デジ/BS/CS 兼用 1）
- torne CECH-ZD1J 1 台（地デジ専用 1）

### 設定例の抜粋

```ini
[common]
lock_dir = /run/bondriver-userland
diagnostics = stderr

[client.px4]
cli_path = /usr/local/bin/px4-ts
runtime_dir = /run/px4-userland

[client.siano]
cli_path = /usr/local/bin/siano-ts
runtime_dir = /run/siano-userland

[client.asicen]
cli_path = /usr/local/bin/asicen-ts
runtime_dir = /run/asicen-userland

[client.pix]
cli_path = /usr/local/bin/pix-ts
runtime_dir = /run/pix-userland

[client.torne]
cli_path = /usr/local/bin/torne-ts
runtime_dir = /run/torne-userland

; 地デジ専用 12 チューナー（EDCB Count = 12）
[group.BonDriver_Userland_T]
tuner_name = Userland-T
channel_table = channels-ground.example.tsv
receivers = siano:s1ud_a:0:T; siano:s1ud_b:0:T; px4:q3u4:2:T; px4:q3u4:3:T; px4:q3u4:6:T; px4:q3u4:7:T; px4:s1ur:0:T; asicen:w3u3:1:T; asicen:w3u3:3:T; asicen:w3u2:1:T; asicen:w3u2:3:T; torne:torne0:0:T

; BS/110度CS専用 8 チューナー（EDCB Count = 8）
[group.BonDriver_Userland_S]
tuner_name = Userland-S
channel_table = channels-satellite.example.tsv
receivers = px4:q3u4:0:S; px4:q3u4:1:S; px4:q3u4:4:S; px4:q3u4:5:S; asicen:w3u3:0:S; asicen:w3u3:2:S; asicen:w3u2:0:S; asicen:w3u2:2:S

; 地デジ/BS/CS兼用 3 チューナー（EDCB Count = 3）
[group.BonDriver_Userland_TS]
tuner_name = Userland-TS
channel_table = channels-all.example.tsv
receivers = px4:m1ur:0:TS; pix:dt181:0:TS; pix:dt295:0:TS

; TVTest / tsdump 用（全 23 チューナー）
[group.BonDriver_Userland]
tuner_name = Userland
channel_table = channels-all.example.tsv
receivers = siano:s1ud_a:0:T; siano:s1ud_b:0:T; px4:q3u4:0:S; px4:q3u4:1:S; px4:q3u4:2:T; px4:q3u4:3:T; px4:q3u4:4:S; px4:q3u4:5:S; px4:q3u4:6:T; px4:q3u4:7:T; px4:s1ur:0:T; px4:m1ur:0:TS; asicen:w3u3:0:S; asicen:w3u3:1:T; asicen:w3u3:2:S; asicen:w3u3:3:T; asicen:w3u2:0:S; asicen:w3u2:1:T; asicen:w3u2:2:S; asicen:w3u2:3:T; pix:dt181:0:TS; pix:dt295:0:TS; torne:torne0:0:T
```

## チャンネル定義ファイル（TSV）

`channel_table` で指定する外部定義ファイルは、タブ区切りの TSV 形式である。

各行に以下のフィールドを記述する（不要な項目には `-` を指定）。なお、ホストアプリケーションから参照される空間番号・チャンネル番号は、TSV ファイル内での出現順（0 始まり）に基づいて割り当てられる。

| 列番号 | 項目名 | 説明 | 例 |
| --- | --- | --- | --- |
| 1 | 空間 ID | 空間識別子（任意の文字列） | `GR`、`BS` |
| 2 | 空間名 | 空間の表示名 | `地上波`、`BS` |
| 3 | チャンネル ID | 空間内の一意なチャンネル識別子 | `T13`、`BS01_0` |
| 4 | サービス表示名 | アプリケーション上で表示される名称 | `T13`、`BS01_0` |
| 5 | 放送方式 | `isdb-t` または `isdb-s` | `isdb-t` |
| 6 | 中心周波数 | 周波数（Hz単位、kHz 丸めなし） | `473142000` |
| 7 | ストリーム ID | TSID（衛星放送で指定。不要時は `-`） | `-` または `16433` |
| 8 | スロット番号 | 衛星放送のスロット番号（不要時は `-`） | `-` または `0` |

用途別のサンプルファイルを `config/` ディレクトリに同梱している。

- 地上波: [config/channels-ground.example.tsv](config/channels-ground.example.tsv)
- BS / 110度CS: [config/channels-satellite.example.tsv](config/channels-satellite.example.tsv)
- CATV: [config/channels-catv.example.tsv](config/channels-catv.example.tsv)
- 地上波 + BS/CS: [config/channels-all.example.tsv](config/channels-all.example.tsv)

## ソースからのビルド

### 必要な環境

- C++17 対応コンパイラ
- CMake 3.16 以上

### ビルド手順

```sh
cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### プラットフォーム固有の留意事項

- **Windows**:
  EDCB や TVTest などの主要なホストアプリケーションは MSVC でビルドされているため、C++ ABI（RTTI や仮想関数テーブル）の互換性を保つために MSVC でのビルドを推奨する。静的 MSVC CRT（`/MT`）でビルドする場合は `-DBONDRIVER_STATIC_CRT=ON` を指定する。なお、`*-ts` CLI プロセスとはプロセス間通信（IPC）で連携するため、CLI 側が MinGW 等でビルドされていてもバイナリの混合リンク問題は発生しない。
- **Linux**:
  glibc 環境向けに GNU ランタイム（libstdc++ / libgcc）を静的リンクする場合は `-DBONDRIVER_STATIC_CXX_RUNTIME=ON` を指定する。musl 環境では musl ツールチェーンを用いて別途ビルドする。
- **macOS**:
  macOS 11.0 以降をターゲットとする場合は `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` を指定する。

詳細なビルドオプションや CI リリースパイプラインの仕様については、[docs/release-build.md](docs/release-build.md) を参照すること。

## ライセンス

本プロジェクトは MIT License のもとで公開されている。詳細は [LICENSE](LICENSE) および [NOTICE](NOTICE) を参照すること。
