# BonDriver_Userland

`BonDriver_Siano` / `BonDriver_PX4` は、既存の userland CLI
([siano-ts](https://github.com/Khronos31/siano-userland) 系 / px4-ts 系) を子プロセスとして起動し、
その標準出力の MPEG-TS を EDCB / TVTest / LibISDB から利用できる BonDriver として
提供する共有ライブラリです。userland 側のソースは変更しません。

## 成果物

| バックエンド | Linux x64 / arm64 | Windows x64 | macOS arm64 |
| --- | --- | --- | --- |
| Siano | `BonDriver_Siano.so` | `BonDriver_Siano.dll` | `BonDriver_Siano.dylib` |
| PX4 | `BonDriver_PX4.so` | `BonDriver_PX4.dll` | `BonDriver_PX4.dylib` |

`lib` 接頭辞は付きません。各ライブラリは次の2つを export します。

- `CreateBonDriver()` → RTTI 付き `IBonDriver2` 派生オブジェクトを新規生成
- `CreateBonStruct()` → EDCB 互換 `STRUCT_IBONDRIVER2`（`const STRUCT_IBONDRIVER*` として返却）

正式な配布対象は Linux x64 / arm64（glibc 2.28 以上または musl 1.2.5 以上）、Windows x64、macOS arm64 です。
Linux は glibc と musl のどちらも、それぞれの対象 libc/toolchain に対してビルドしてください。古い全 Linux 環境や
未記載の ABI baseline までの互換性は保証しません。

## ビルドとテスト

```sh
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
```

隔離 fixture と CTest も実行する場合は、

```sh
cmake -S . -B build-tests -DBUILD_TESTING=ON
cmake --build build-tests -j2
ctest --test-dir build-tests --output-on-failure
```

- C++17 / CMake。
- テストは実機・カード・実 daemon に接続しません。`tests/fake_cli` のみを子プロセスとして使います。
- glibc 2.28 / musl 1.2.5 の最低バージョンを対象にする場合、対応する sysroot/toolchain でビルドしてください。
  新しいホスト上で通常ビルドするだけでは古い libc の baseline にはなりません。
- 再現可能な release build、Linux の任意 static C++ runtime、Windows の任意 static CRT は
  [docs/release-build.md](docs/release-build.md) を参照してください。

## 設定

ライブラリと同じ basename の INI を、ライブラリ自身の場所から読みます
（`BonDriver_Siano.so` → `BonDriver_Siano.ini`）。INI 内の相対パスは
INI のあるディレクトリ基準で解決し、アプリの cwd には依存しません。

試験用に限り、環境変数 `BONDRIVER_SIANO_CONFIG` / `BONDRIVER_PX4_CONFIG`
（または `BONDRIVER_CONFIG`）で絶対パスの明示指定ができます。本番設定とは分離して使います。

### 共通セクション `[common]`

| キー | 必須 | 意味 |
| --- | --- | --- |
| `tuner_name` | yes | `GetTunerName` が返す UTF-8 名 |
| `cli_path` | yes | 起動する CLI のパス（shell を介さず argv 配列で起動） |
| `channel_table` | yes | 外部チャンネル表（後述） |
| `tune_timeout_ms` | no | 選局後、最初の TS を待つ上限（既定 4000） |
| `stop_timeout_ms` | no | 正常終了待ち（SIGTERM 相当）の上限（既定 5000） |
| `kill_timeout_ms` | no | 強制終了後の回収待ち上限（既定 2000） |
| `buffer_limit_bytes` | no | TS キューの上限（既定 8388608） |
| `diagnostics` | no | `stderr` / `discard` / `file:PATH`（既定 `stderr`）。`stderr` は子の診断を drain して内部 bounded tail に保持します。`file:` は通常ファイルへ子 stderr を直接追記し、FIFO 等の非通常ファイルは拒否します |
| `lock_dir` | no | プロセス間 lease のディレクトリ。既定は Linux: `$XDG_RUNTIME_DIR` または `/run`、macOS: `$TMPDIR` または `/tmp`、Windows: `%ProgramData%` 配下。隔離テストは専用ディレクトリを明示します。Termux では書込み可能な private directory を設定してください |

### Siano セクション `[siano]`

| キー | 意味 |
| --- | --- |
| `device` | 固定 selector（1台を占有） |
| `devices` | プール selector のカンマ区切り。空きを順に確保 |
| `firmware` | 任意。指定時は `--firmware PATH` を付与 |

`device` と `devices` はどちらか一方のみ必須です。selector は
`siano-ts --device` が解釈する正準表記をそのまま使います。同一の物理デバイスを
別 selector で表す alias は初期非対応で、検出・重複排除しません。

### PX4 セクション `[px4]`

| キー | 意味 |
| --- | --- |
| `runtime_dir` | 稼働中 `px4d` のランタイムディレクトリ（必須） |
| `instance` + `receiver` (+ `systems`) | 固定受信機。`systems` は `T`/`S`/`TS`（既定 `TS`） |
| `receivers` | プール。`instance:receiver:systems` を `;` で列挙 |
| `lnb_voltage` | `0` または `15`（既定 0） |
| `allow_lnb_15` | `lnb_voltage = 15` のとき `1` が必要 |

`px4-ts` は起動済み daemon に接続します。daemon の自動起動・終了は行いません。
`runtime_dir` は**実在**している必要があり、ファイルシステムの正準パス（symlink/junction を解決）に
正規化して lease キーに使います。別表記の alias が同じ実体を指す場合は同一 identity として排他し、
解決できないパスは読込み時に失敗させます（lexical な別 identity にしません）。

### 検証

設定の読込み時に次を拒否します: 未知キー、同一セクション内の重複キー、必須キー欠落、
範囲外の値、`device`/`devices` や固定/プールの同時指定、PX4 の重複 receiver、
`lnb_voltage=15` かつ許可なし、チャンネル側の非対応 system など。

## 外部チャンネル表

8列の TSV（タブ区切り）です。`#` で始まる行はコメント、`space_id` 行はヘッダです。
列は
`space_id` / `space_name` / `channel_id` / `channel_name` / `system` / `frequency_hz` / `stream_id` / `slot`
です。空欄は `-`。

- `system` は `isdb-t` または `isdb-s`。
- `frequency_hz` は Hz 単位。ISDB-T は 90〜1000 MHz、ISDB-S は 950〜3500 MHz の範囲で検証します
  （kHz 等の混在単位は範囲外として拒否）。
- ISDB-S は `stream_id` か `slot` のどちらか一方が必須。両方指定は競合として拒否。
- ISDB-T は `stream_id`/`slot` を指定できません。
- `channel_id` は全体で一意。`space_id` の出現順と `channel_id` の並びは安定で、勝手に詰め替えません。
- バックエンドが対応しない system を含む表は読込み時に失敗します
  （Siano は isdb-t のみ。PX4 は設定した受信機能力に従う）。

`config/` に地上波 13–62、BS、110度CS、CATV（任意周波数）の例があります。

## CLI への変換

- Siano: `siano-ts --device SELECTOR [--firmware PATH] --freq HZ`
- PX4: `px4-ts --instance TOKEN --receiver N --system isdb-t|isdb-s --frequency-khz KHZ
  --runtime-dir PATH --output - [ISDB-S は --stream-id TSID または --slot N] [--lnb-voltage 0|15]`

周波数はチャンネル表の Hz から各 CLI の単位へ変換します（PX4 は kHz に丸め）。
子は shell を介さず argv 配列で直接起動します。

## 受信機の確保と排他

- `OpenTuner` で受信機 lease を取得します（固定指定はその1台、プールは空きを選択）。
- 選局する system に現在の受信機が非対応なら、プールから選び直します。
  専用機を優先し、T/S 共用機は最後に回します。
- lease は受信機 identity（Siano は selector、PX4 は正準化 runtime + instance + receiver）を
  キーにしたファイルロック（POSIX: `flock`）です。これは本ライブラリ利用者間の協調排他で、
  任意の外部 CLI や他ソフトの同時利用を排除するものではありません。
- 同一プロセス内で複数の factory を呼んでも、同じ受信機を二重に確保しません。
  `CreateBonStruct` はライブラリごとの singleton で、未解放の間は同一構造体を返します。

## TS と ABI の契約

- 188 バイト MPEG-TS パケット境界を保ちます。分割 read は再構成し、末尾の端数は完成するまで露出しません。
- `WaitTsStream` は `WAIT_OBJECT_0(0)` / `WAIT_TIMEOUT(258)` / `WAIT_FAILED(0xffffffff)` を返します。
  子の死亡・EOF 後は `WAIT_FAILED` を返し、無期限待機を残しません。
  `INFINITE(0xffffffff)` を含む長い timeout は、Close/停止を妨げないよう内部で有限（最大
  5000 ms）に丸めます。これは API の待機を有界ポーリングとして扱う意味です。
- `GetReadyCount` と `GetTsStream` の `remain` は**取得可能な獲得バッファ数**です。byte 数でも
  188 バイトパケット数でもありません。1 回の `GetTsStream` は獲得バッファを 1 個だけ返し、
  `remain` はこの取得後に残っている獲得バッファ数です。
- 獲得バッファは 188 の倍数で、公開最大長は `kMaxTsBlockBytes = kMaxGetTsStreamBytes = 188 * 512 = 96256`
  バイトです。実際の長さはそのときに到達したデータ量に依存し、最大長を超えません。
- copy 版 `GetTsStream(BYTE*, DWORD*, DWORD*)` の `*size` は**出力**です。入力容量としては読みません。
  **copy 版の呼出し側は少なくとも `kMaxGetTsStreamBytes` のバッファを用意する必要があります**
  （実装は未初期化 `*size` を容量として扱いません）。
- pointer 版 `GetTsStream(BYTE**, ...)` が返すポインタは、次の取得・破棄・終了まで有効です。
- `PurgeTsStream` は世代境界を作ります。古い partial を破棄し、OS pipe 内に残った選局前 TS を
  確実に除くため、同じ受信機 lease を保持したまま同じチャンネルの子を有限時間で再起動します。
  再起動に失敗した場合は受信失敗状態にし、旧 TS を戻しません。
- `GetSignalLevel` は `0.0f` を返します。現 CLI 契約では CNR を取得できないためで、
  **0 は CNR 不明を意味します**。固定の架空値は返しません。
- 有限キューが上限に達した場合は 188 バイト単位で古い側から drop し、drop 数/バイト数を記録します。

## 子プロセス

- POSIX は `posix_spawn`（fork 後の非 async-safe 処理なし）、stdout=TS / stderr=診断、
  stdin は `/dev/null`。reader は非ブロッキング fd と poll、専用 stop pipe で有限に join します。
- 終了は SIGTERM → 期限後 SIGKILL で、所有する子だけを回収します。子孫が pipe を保持しても
  reader は stop pipe で解けます。所有外プロセスは終了しません。
- Windows は `CreateProcessW` を継承ハンドルリスト付きで起動し、Job object（KILL_ON_JOB_CLOSE）で
  子孫を含む後始末を有限化します。Job の作成/設定/割当と Resume の失敗は起動失敗として扱います。
- reader の stdout/stderr 取込みは 1 poll あたりの量を制限し、連続 flood 中でも停止指示を観測します。
- `stderr` はホストの stderr へは書かず、子 stderr pipe を drain して最大 8 KiB の内部 tail に保持します。
  この tail は現在 BonDriver の公開 ABI からは読めません。host fd の状態を変えず、reader から外部 sink への書込みもしません。
- `file:PATH` は通常ファイルを子の stderr に直接割り当てます。FIFO / named pipe 等は受け付けません。
  `discard` は子 stderr を破棄します。ホストのコンソール制御イベントは送出しません。
- 停止時は子の stdout を閉じることで、出力失敗で自発終了する CLI を正常終了させられます
  （fake fixture で確認）。

## 検証状況

- 現行の独立 CTest 39件は、Linux glibc 2.28 x64（9.24秒）、Linux glibc 2.28 arm64（QEMU/PRoot、15.04秒）、
  Alpine Linux musl 1.2.5 x64（9.40秒）、Alpine Linux musl 1.2.5 arm64（QEMU、14.29秒）、macOS arm64 ネイティブ（8.82秒）、
  Windows x64/MSVC（static CRT、成功）で通過しました。
- Linux x64 の ASan/UBSan も `ASAN_OPTIONS=detect_leaks=1` と `UBSAN_OPTIONS=halt_on_error=1` で 39/39（6.92秒）でした。
- `BUILD_TESTING=OFF` の release build は Linux glibc x64/arm64、musl x64/arm64、Windows x64、macOS arm64 で完了しました。
  独立した動的ランタイム consumer による EDCB ABI 79項目と LibISDB ABI 26項目も、全4 Linux構成の両バックエンドで成功しています。
- Linux の static C++ runtime を使った両 DSO の load-order/isolation 試験では、native glibc x64、異なる GNU runtime を使う x64 consumer、
  glibc arm64（QEMU/PRoot）、musl x64（PRoot）、musl arm64（QEMU）で、local/global load、両順序、IBonDriver2 cast と無関係 interface cast の拒否、並行 TS 取得が成功しました。
  ただし glibc x64 と musl arm64 の PRoot では断続的な process spawn failure が観測されています。直接 loader 実行と raw QEMU 実行は通過しましたが、
  PRoot failure の原因は未確定です。
- Linux arm64 は QEMU/PRoot または QEMU による隔離エミュレーション検証です。arm64 実機でのネイティブ実行は未実施です。
- Windows x64 の static CRT DLL は依存 import が KERNEL32 のみであることを確認しました。通常の動的 CRT を使う独立 consumer でも
  EDCB ABI 79項目と LibISDB ABI 26項目が成功しています。
- macOS arm64 は deployment target 11.0 で release build され、現在の macOS 26 上で同じ独立 ABI consumer が成功しました。
  EDCB ABI 79項目と LibISDB ABI 26項目が成功しています。macOS 11 実機/OS 上での実行は未検証です。
- Windows x86 も追加検証しましたが、正式な配布対象ではありません。
- FreeBSD は Zig/Clang 21 による両 DSO の cross compile/link と WERROR が成功しましたが、native build / runtime test は未実施です。
  さらに両 backend の独立 EDCB/LibISDB consumer translation unit（79/26 assertion suite）が compile しましたが、FreeBSD 上の runtime 実行は未実施です。
  Termux / Android は compile/runtime とも未検証です。
  Termux を試す場合は Android API 28 以上を対象にし、`lock_dir` と必要なら `TMPDIR` に書込み可能な private directory を指定してください。
- Windows PX4 は、将来同じ CLI オプションと TS 出力契約を持つ `px4-ts` が提供されることを前提とする先行対応です。
  Windows PX4 CLI integration は未検証です。実 EDCB / TVTest アプリ本体、受信機・カード・daemon を使った受入試験も未実施です。
- CNR は現在の CLI 契約では取得できないため、値は不明です。

ライセンスとコードの出自は [NOTICE](NOTICE) を参照してください。
