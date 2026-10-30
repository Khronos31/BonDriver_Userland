# リリース向けビルド手順

リリース用成果物をビルドする際は、`-DBUILD_TESTING=OFF` を指定する。既定ではコンパイラ標準の C++ ランタイムリンクを使用する。対象ターゲットに合致するツールチェーン環境から CMake を実行すること。新しいホスト上で新しいコンパイラを使用しても、それだけで libc の ABI 基準バージョンが下がるわけではない点に留意すること。

## Linux

サポート対象の下限とする glibc 環境向けには、glibc 2.28 以下の sysroot とコンパイラを使用し、生成された動的シンボルのバージョンがその範囲に収まっていることを確認すること。musl 向けには、musl 1.2.5 に基づくコンパイラおよび sysroot を使用する。x64 と arm64 はそれぞれ個別にビルドする。

```sh
cmake -S . -B build-linux \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel 2
```

クロスコンパイラを使用する場合は、`-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake` のようにツールチェーンファイルと適切な sysroot を渡す。異なる libc ファミリ間やアーキテクチャ間でビルドディレクトリを再利用してはならない。

Linux ELF ビルドでは、オプションとして GNU 互換の静的 C++ / unwind ランタイムリンクを利用できる。

```sh
cmake -S . -B build-linux-static-cxx \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DBONDRIVER_STATIC_CXX_RUNTIME=ON
cmake --build build-linux-static-cxx --parallel 2
```

このオプションは、GNU または Clang の Linux ELF ツールチェーンで利用できる。libstdc++ および libgcc / unwind のサポートコードをドライバ本体に静的リンクし、エクスポートマップを適用して内部実装シンボルを非公開としつつ、BonDriver のファクトリ関数およびインターフェース RTTI を外部へ公開する。libc などのシステム依存部分は動的リンクのまま保持される。このオプションは glibc 向けバイナリを musl 上で動作可能にするものではなく、libc の ABI 基準バージョンを変更するものでもない点に留意すること。

## Windows x64

既定ではツールチェーン標準の動的 MSVC ランタイムを使用する。静的 MSVC CRT（`/MT`）を用いて製品 DLL をビルドする場合は、専用のビルドディレクトリを用意して以下のようにビルドする。

```powershell
cmake -S . -B build-windows-static-crt `
  -DBUILD_TESTING=OFF `
  -DBONDRIVER_STATIC_CRT=ON
cmake --build build-windows-static-crt --config Release --parallel 2
```

`BONDRIVER_STATIC_CRT` は `BonDriver_Userland.dll` に適用される。既定では OFF であり、個別にビルドされるコンシューマ側のランタイム設定には影響しない。ドライバが所有するオブジェクトの生成と破棄は、ファクトリ関数および `Release()` インターフェースを通じて必ず DLL 内部で完結させること。

## macOS arm64

macOS SDK および AppleClang を使用してネイティブビルドする。ビルドホストの新しい既定バージョンを暗黙に継承しないよう、デプロイメントターゲットを明示的に指定すること。

```sh
cmake -S . -B build-macos \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos --parallel 2
```

デプロイメントターゲット 11.0 でのビルドおよび独立した ABI コンシューマの動作は macOS 26 上で確認している（macOS 11 実機での動作検証は未実施）。なお、Linux 向けの静的 C++ ランタイムオプションは macOS には適用されない。

いずれの構成でも、ビルドツリー内には `BonDriver_Userland` が生成される。出力ファイル名は Linux では `.so`、Windows では `.dll`、macOS では `.dylib` となる。追加の `BonDriver_Userland*` は同一ライブラリの実体コピーであり、モジュールの basename に応じて共通 INI 内の対応する `[group.NAME]` が選択される。

## GitHub Actions リリースパイプライン

`.github/workflows/release.yml` は、標準の GitHub ホステッドランナー上で対応 6 構成をビルドする。Linux ビルドは、ホストランナー上でチェックアウトした後にダイジェスト固定された glibc 2.28 または Alpine / musl 1.2.5 のコンテナイメージ内で実行される。ARM64 ジョブはネイティブの ARM64 ランナーを使用する。プルリクエスト、`main` へのプッシュ、および手動実行により、ビルド、テスト、リリース向け検査、および CI アーティファクトのアップロードが実行される。

各プラットフォームジョブは、まず `BUILD_TESTING=ON` で全 30 件の CTest を実行し、続いて `BUILD_TESTING=OFF` でビルドした製品バイナリをコンシューマテストディレクトリに配置して、耐障害性テスト（fault-injection）を除く 26 件のテストを実行する。Linux ジョブでは ELF アーキテクチャ、glibc ジョブにおける GLIBC 2.28 シンボルバージョン上限、動的 libstdc++ / libgcc の非依存性、および正確なファクトリ / インターフェース RTTI エクスポートセットを検証する。Windows では x64 および静的 CRT インポート境界を、macOS では arm64、デプロイメントターゲット 11.0、およびシステム動的依存のみであることをそれぞれ検証する。各アーカイブには、ドライバ本体、代表設定例、プロジェクトのライセンスファイル群、`SHA256SUMS`、およびコミットハッシュやツールチェーン情報を記載した `BUILD-INFO.txt` が同梱される。

セマンティックバージョニング形式のタグ（`vMAJOR.MINOR.PATCH`、プレリリースやビルドメタデータを含む形式も可）がプッシュされた場合にのみ、GitHub Release への公開処理が実行される。公開ジョブはマトリックスの全構成の完了を待ち、タグがチェックアウトコミットと一致することを確認したうえで、正確に 6 つのプラットフォームアーカイブを要求し、統合 `SHA256SUMS` を生成してドラフトリリースを作成・公開する。既存のリリースが存在する場合は上書きせずジョブは失敗するため、公開済みタグを再実行してもアセットが置き換わることはない。
