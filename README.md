# RouteLine

CS2KZのGlobal・サーバー内順位のリプレイを、ゲーム内のルート線として表示するLinux用Metamodプラグインです。v0.13.0からWorkshop描画専用です。RouteLineのWorkshop素材が必要です。

## v0.16.13の表示

踏み切り位置を水平な×印で表示します。Classicで着地から20 ms以内の踏み切りと推定できたものは緑、通常のジャンプやタイミングを確定できないものは黄橙色です。リプレイにはパーフェクト判定そのものが保存されていないため、Vanillaの×印は黄橙色で表示します。

`!line preview` で青・赤・青の接地枠と黄橙・緑の×印を確認できます。×印をサーバー設定で無効にするには `render.takeoff_markers: false` を設定します。

Classicで同じtickの中に着地と踏み切りが収まるBhopは、落下軌道と踏み切り後の速度から接地を補います。半tickより早く足場に当たった場合や、下降速度が小さい場合も対象です。

`render.hide_failed_attempts`（既定 `true`）は、同じ座標への連続したTPや、記録されたチェックポイントへの戻りでやり直した区間をガイドから外します。最初の進入と最後の試行で足場から離れた直後の動きが一致した場合は、その地点を境に最後の試行へ切り替えます。`!line status` の `Retry frames hidden` で、表示対象から外したフレーム数を確認できます。

判定に必要な記録がない区間は表示に残ります。初めての地点へ移るTPの前後には、記録に沿って別々の線を描きます。すべての試行を見たい場合は `hide_failed_attempts: false` を設定してください。


配布するプロファイルは `workshop-flat` の1つです。起動時に選ばれ、ルート線はpacked描画、着地枠は平面素材で表示します。タイマーの種類は `provider` で指定します。

通常線は白、しゃがみ区間は赤です。着地枠は通常接地が青、しゃがみ接地が赤です。既定はFollowです。`!line render follow` で追従、`!line render all` で全体表示に切り替えます。どちらも読み込み前に選べます。`!line status` の `Display` で現在の方式とFollowのフレーム上限・切り替え間隔を確認できます。

空中からラダーにつかまった位置も枠で表示します。ラダーの枠はリプレイでラダー状態になった位置を基準にし、付近の床への位置補正を省きます。

白と赤の線の粒子間隔は `route_particle_spacing`、連続する同色の線をまとめるフレーム数は `path_batch_frames` で調整できます。標準は20ユニットと144フレームで、粒子間隔は100ユニットまで指定できます。ロープ式の着地枠の粒子間隔は標準で8ユニットです。表示範囲の端では、パスのまとめ方により描画が終わる位置が少し変わる場合があります。

標準のFollowでは、現在位置からラインは1,200ユニット、平面のジャンプ印と接地枠は600ユニットを目安に表示します。表示中のグループは境界でちらつかないよう、最大128ユニット余分に残します。

描画距離を設定しているFollowでは、範囲の端でリプレイ順の次のパスを1本だけ先に表示できます。そのパスに対応する近くの接地枠も、1エフェクト分まで先に表示します。先行表示には距離に最大256ユニットの余裕を持たせ、設定されたフレーム上限に収めます。次の区間が現在の線に沿って同方向に通り直す場合や、チェックポイント復帰後に線が重なる場合は、近づいた時点で次の線へ切り替えます。この切り替えで先に出した区間の枠は、通常のFollowで区間を選んだ時点から表示します。

packed描画は白と赤の連続区間を頂点色で1本のパスにまとめます。テレポートや記録の切れ目ではパスを分けます。通常線としゃがみ線のアルファを違う値に設定した場合も、透明度を保つため色ごとに分けます。

v0.16.18のWorkshop素材では、接地枠と×印を20×20の平面モデルで描きます。OFFや順位変更で対応するエンティティを削除します。`!line status` の `Marker renderer: mesh` で新方式を確認できます。旧素材を読み込んだマップでは、従来の平面パーティクルを使います。更新手順は [WORKSHOP.md](WORKSHOP.md) を参照してください。

## 表示範囲

全体表示は、位置による範囲切り替えを停止して、全区間のパスと接地枠を保持します。パスは既定で1サーバーフレームに最大4個ずつ生成します。サーバー全体の `max_total_elements` を超える場合は切り替えを断り、現在の表示を保持します。長い記録ではFollowより多くのパスを保持するため、FPSへの影響はゲーム内で比較してください。

Followでは位置に最も近い連続区間から最大3,000フレームを選び、1,500フレーム進むごとに次の範囲へ切り替えます。重なる1,500フレーム分のパスは再利用します。短い区間や終端では範囲を区間内に収めます。観戦中は観戦対象に追従します。`render.separate_sections`（既定 `true`）でTP前後の範囲を分け、再試行の切り替え地点だけはその前後を同じ範囲に含めます。各プレイヤーの表示は独立し、OFF・切断・マップ変更・設定リロード時に消去します。

ルート線は記録軌跡を近似したガイドで、壁に対する経路探索は行いません。接地枠は20×20の水平な四角です。記録された接地点の近くで足場を探す位置補正は残し、傾斜・縮小・短線への代替をやめました。枠の中心は足場の上2.5ユニット、足場が見つからなければ記録位置＋ `z_offset`（既定2.5）です。空中やはみ出しを許容します。明示的な `compact` / `landings` の短線表示は維持します。

## 必要なもの

- RouteLine Workshopアドオン（導入は [WORKSHOP.md](WORKSHOP.md)）
- Linux x86_64のCS2 Dedicated Server
- KHook搭載のMetamod（プラグインAPI 18）
- `ICS2KZ002` を公開するCS2KZ。参照リビジョンは [6f7ad5e](https://github.com/KZGlobalTeam/cs2kz-metamod/tree/6f7ad5eb778cc80cfc1f31c330084616ffb753bc)
- SR表示にはCS2KZで使っているSQLMM・SQLite／MySQL DBと対象SRのリプレイファイル

## 接続先はタイマーのcfgから読む

CS2KZ用アダプターは `csgo/cfg/cs2kz-server-config.txt` をValveのKeyValuesパーサーで読みます。RouteLine側にDB接続情報やGlobalの接続先を複製する必要はありません。

| CS2KZの設定 | 用途 |
| --- | --- |
| `apiUrl` | Globalのマップ・コース・順位別記録の取得 |
| `replayDownloadUrl` | リプレイ取得先。`{id}` を記録UUIDに置換 |
| `db.driver` / `db.database` | CS2KZと同じSQLite／MySQL DB |
| `db.host` / `db.user` / `db.pass` / `db.port` / `db.timeout` | MySQL接続 |

SQLiteの保存先もCS2KZと同じ `addons/cs2kz/data/{database}.sqlite3` です。DBにはSELECTだけを発行します。Globalの公開GETに `apiKey` は使いません。接続先を変更したら、サーバーコンソール／RCONで `routeline_reload` を実行してください。

## 導入・更新

1. 新規導入ではZIPの `addons/` と `cfg/` をサーバーの `game/csgo/` に配置します。
2. 更新時は旧版をアンロード、またはサーバーを停止してから `routeline.so` を差し替えます。既存の `config.json` では平面枠のプロファイルを `workshop-flat` に改名し、`active_profile` に同じ名前を指定します。配布設定に置き換える場合は `reload_admins` や使用中の設定値を引き継いでください。旧 `cfg/routeline.cfg` でプロファイル名を指定している場合は、同梱のcfgに差し替えます。
3. `meta list` で導入したバージョンを確認します。広い平らな場所で `!line preview` を実行すると、現在の設定で線と3つの着地マーカーを確認できます。
4. `!line off` でプレビューを消し、`!line global pro` または `!line sr pro` を実行します。

`!line status` は表示フレーム範囲・パス数・線幅・実際に読み込んだRGB値を表示します。通常線が青い場合は `RGB route` と設定ファイルを確認してください。配布サンプルの通常線は `255/255/255` です。

`path_style` がなく、0.6の既定値（横6・高さ1・最大長128）のままなら、新しい既定値（横6・空中の高さ3・最大長256）に移行します。独自の誤差設定や、明示した `path_style` は維持します。

## コマンド

`!line o` または `!line options` で設定メニューを開きます。表示方式はCS2Menusのサーバー設定とプレイヤーの設定に従います。一時的に指定する場合は `!line options panorama`、`!line options html`、`!line options chat` を使います。実際に選ばれた方式はCS2の開発者コンソールに出ます。[CS2Menus（ICS2Menus008）](https://github.com/FemboyKZ/mm-cs2menus)

表示範囲（Follow / All）と、Line・Jump marks・Landing marksのスイッチは、表示中のルートに反映されます。記録元・カテゴリ・モード・順位は「Show / replace route」を選ぶと反映されます。選んだ条件は同じマップ内の引数なしの `!line` にも使います。HUDから読み込むコースは現在のコースです。名前で別コースを指定するときはチャットコマンドを使います。

| チャット | 動作 |
| --- | --- |
| `!line` / `/line` | HUDで選んだ条件または既定条件で表示。表示中・読み込み中はOFF |
| `!line o` / `!line options` | 個人設定のHUDを表示 |
| `!line global pro` | GlobalのPRO 1位 |
| `!line g 2` | Globalの2位。カテゴリとモードは省略時の設定を使用 |
| `!line sr 2` | サーバー内の2位 |
| `!line sr overall` | ローカルDBのOverall SR |
| `!line global pro vnl "Main"` | モードとコースを指定 |
| `!line preview` | 細線・枠の確認用表示 |
| `!line render all` | ルート全体を固定表示 |
| `!line render follow` | 進行に合わせて表示範囲を切り替え |
| `!line render line on/off` | ラインの表示を切り替え |
| `!line render jump on/off` | ジャンプ印の表示を切り替え |
| `!line render landing on/off` | 接地枠の表示を切り替え |
| `!line reload [プロファイル名]` | 管理者が設定を再読み込み。省略時は現在のプロファイル |
| `!line off` | 表示終了 |
| `!line status` / `!line help` | 状態・RGB／使い方 |

コンソールでは `routeline global pro ckz "main"` のように使えます。`global` は `wr`、`sr` は `local`、`ckz` は `classic`、`vnl` は `vanilla` でも指定できます。`overall` はテレポート有無を問いません。

モード省略時は現在のモード、コース省略時は実行中のコース、それがなければタイマー側の最初のコースを使います。コース名の大小文字はそのまま扱い、スペースを含む名前は引用符で囲みます。別のコースへ移った場合は選び直してください。

順位は `g`、`global`、`sr` などの直後に1～1,000で指定します。`!line sr 3 overall` のようにカテゴリも指定できます。省略時は1位です。順位はコース・モード・カテゴリごとに、プレイヤーごとの自己ベストを速い順に数えます。選んだ記録のリプレイが欠損・破損している場合は、次の順位を指定し直してください。SRは `kzreplays/{UUID}.replay` と `kzreplays/downloads/{UUID}.replay` を探します。Globalも同じUUIDのローカルファイルがあれば利用し、なければ設定先から取得します。

## 表示設定・プロファイル

`addons/routeline/config.json` の `active_profile` は標準で `workshop-flat` です。`provider` は現在 `cs2kz`、`source` は引数を省略したときの記録元（`global` / `sr`）、`category` は `pro` / `overall` です。`!line g` と `!line sr` で記録元を選べます。

| render項目 | 既定値 | 用途 |
| --- | --- | --- |
| `mode` | `"persistent"` | 選択範囲のパスを保持 |
| `renderer` | `"workshop"` | Workshop専用。旧描画方式の値は互換読み込み |
| `detail` | `"full"` | ルート線と四辺枠。`compact` はルート線と短い青線、`landings` は短い青線のみ |
| `markers_only` | `false` | 旧設定。trueの場合、初期状態では接地枠のスイッチだけがON |
| `follow` | `true` | `true` は追従表示、`false` は全体表示 |
| `replay_frames` | `3000` | Followの表示フレーム上限。1～3000 |
| `window_overlap` | `1500`（3,000フレーム時） | 切り替え時に重ねるフレーム数。省略時は `replay_frames` の半分（端数切り捨て）。`0` は現在位置を中心とする方式 |
| `window_interval` | `0.5` | 範囲選択の更新秒数。0.1～2 |
| `route_batch` | `"standard"` | `"packed"` で白・赤を頂点色で1本にまとめる |
| `path_batch_length` | `1280`／packedは`1600` | パスの最大長。標準は640～1280、packedは640～2048 |
| `path_batch_frames` | `144`／packedは`320` | パスをまとめるフレーム数。標準は1～256、packedは1～512。実際の上限は `replay_frames` の1/4以下 |
| `route_particle_spacing` | `20` | Workshopのルート線の粒子間隔。1～100ユニット |
| `route_distance` | 配布cfgは `1200`、省略時は `0` | Follow中にラインを追加する距離。0～8192ユニット。`0` は距離制限なし |
| `marker_distance` | 配布cfgは `600` | Follow中に平面のジャンプ印と接地枠を追加する距離。0～8192ユニット。省略時は `route_distance` と600の小さい方。`0` は距離制限なし |
| `marker_particle_spacing` | `8` | Workshopの着地枠の粒子間隔。1～8ユニット |
| `beam_width` | `0.5` | 始点・終点の線幅。0.1～4 |
| `path_style` | `"smooth"` | 曲線近似。`precise` で前版の折れ線方式 |
| `curve_flatness` | `3` | 曲線の描画分割・線の統合の基準。0.25～8 |
| `path_xy_error` | `6` | 曲線近似の横方向の誤差。0.1～16 |
| `path_z_error` | `3` | 空中の曲線近似の高さ誤差。0.1～4 |
| `max_path_length` | `256` | ルート線1本の最大長。32～512 |
| `landing_markers` | `true` | 接地枠の表示 |
| `takeoff_markers` | `true` | 通常表示に踏み切りの×印を追加 |
| `marker_render` | `"rope"` | `"flat"` は水平の平面素材。新しいWorkshop素材が必要。短線表示はロープ式 |
| `landing_size` | 廃止 | 旧設定の値は無視し、四角枠は常に20×20 |
| `landing_width` | `0.85` | ロープ式の接地枠の太さ。0.5～1.25。平面枠の線幅は素材で固定 |
| `z_offset` | `2.5` | 記録座標からの高さ |
| `route_color` | `[255,255,255,255]` | 通常線の白 |
| `landing_color` | `[40,140,255,255]` | 接地枠の青 |
| `crouch_color` | `[255,40,40,255]` | しゃがみ区間としゃがみ接地枠の赤 |
| `max_total_elements` | `4096` | サーバー全体の選択パス数の上限 |
| `create_per_frame` | `4` | サーバーの1フレームで生成する上限 |

色はRGB／RGBAです。旧 `color` は `route_color`、`jump_color` は `landing_color`、`max_total_beams` は `max_total_elements` の別名として読み、新しいキーを優先します。旧 `jump_markers` / `max_jumps` はそれぞれ `landing_markers` / `max_landings` の別名です。

`limits` は読み込みサイズ、展開サイズ、tick数、解析用線分数、接地数などの安全上限です。`cache_routes` はデコード済みリプレイのLRUキャッシュ数です。マップ変更・設定切り替えでキャッシュを破棄します。

チャットから再読み込みするには、`addons/routeline/config.json` のトップレベルに `"reload_admins": ["76561198000000000"]` を追加し、例のIDを自分のSteamID64に置き換えます。IDは引用符で囲みます。最初の登録はサーバー再起動か、サーバーコンソール／RCONの `routeline_reload` で反映します。

- `!line reload` は現在のプロファイルを読み直します。
- `!line reload workshop-flat` は標準プロファイルを指定して再読み込みします。
- 再読み込みに成功すると全プレイヤーのRouteLine表示が消えます。`!line` などで表示し直すと新しい設定が適用されます。設定の検証に失敗した場合は、稼働中の設定と表示を保持します。

サーバーコンソール／RCONでは、引数を省略すると `active_profile` を読み込みます。

```text
routeline_reload
```

`cfg/routeline.cfg` はプラグインのロード後にサーバー側で `exec` する例です。

## 記録の扱いと制限

- CS2KZ replay v1～v7の差分・zstdを解析し、ヘッダーのマップ・コース・モード・カテゴリ・記録時間を照合します。開始前／終了後のバッファを除外します。マップMD5の実サーバー照合は未実装です。
- 接地フラグが空中から接地へ変わった記録を採用し、区間の先頭が接地中なら開始アンカーにします。Classicの同一tick内のBhopは、前後の速度と位置が整合する場合に補います。ほかの一瞬の接地候補は、リプレイ読み込み時に足元の幅で衝突判定を行い、近い高さに立てる面がある場合に枠を追加します。
- CP／テレポート、tickの不連続、大きな座標の飛びで区間を分け、区間をまたいで接地点を結びません。
- ルート線には、粗い採用間隔の解析用線分とは別に、記録された全pre/post座標から作った簡略軌跡を使います。`limits.point_spacing` は描画するルートの座標を間引きません。smooth方式では `path_xy_error` / `path_z_error` が曲線近似、`curve_flatness` が描画分割の基準です。最終表示には両段階の誤差が含まれます。地面への投影・衝突トレースは行いません。
- 赤色は入力ボタン・duck状態・duckAmountから得たしゃがみ区間です。曲線上の対応位置で色を変え、色の切り替えだけを理由に幾何形状を折り曲げません。
- 読み込み・展開・位置変換はバックグラウンドジョブ1件で行います。SteamID、リクエスト世代、マップ世代を照合して古い結果を適用しません。同時デコード中の別要求には再試行を案内します。
- 表示中のWRは次に表示を要求したときに取得し直します。メニュー、PB指定、設定の永続化は未実装です。

## 他タイマーへの拡張

`IRouteProvider` が記録・入力データ・専用デコーダーを供給し、共通の `Route` に変換します。描画側はDBスキーマや圧縮形式を参照しません。SharpTimer／surf-metamod用アダプターは未実装で、追加時も各タイマー自身の設定から接続先を読む方針です。動的な第三者製拡張を読み込む安定ABIはありません。

## ビルド・確認

LinuxのCMake 3.16以上、C++20コンパイラー、make、Git、Python 3が必要です。依存のリビジョンは `bootstrap.sh` に固定しています。配布バイナリはSteamRT sniper SDKでビルドします。

v0.16.19はCS2 1.41.9.7のメッセージAPI変更に対応し、zer0k-z版S2SDKの `9c6974a8b507cada425a395fe9a4125bf4be6fe7` を使います。チャット通知は `UM_TextMsg` のIDで取得します。サーバーを停止して新しい `routeline.so` に差し替えてください。cfgとv0.16.18のWorkshop素材は引き続き使えます。

```sh
bash bootstrap.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/stage"
python3 scripts/package.py --build build
```

解析ツールのみなら `-DLINE_BUILD_PLUGIN=OFF` を指定できます。

```sh
./build/replay_inspect /path/to/record.replay kz_map "Main" classic pro
```

自動テストは形式別フィクスチャ、破損・切断・上限・キャンセル、観測された接地、しゃがみ、TP分断、四辺枠、曲がり角・段差・ジャンプの弧の保持、細かな左右の振れの間引き、全記録点に対する横／高さ誤差、S字の曲線・色分け・分割数・固定点、曲線部分区間の一致、3,000フレームの選択と1,500フレームでの切り替え、空間検索、範囲の重複共有・切断・容量不足、送信制御、PRO／OverallのSQL、元cfgの解釈、実バイナリのMetamod API 18を確認します。実際の線の見え方と負荷は実サーバーで別途確認が必要です。詳細は `VALIDATION.md` を参照してください。

## 参考・謝辞

リプレイの走行経路を線で表示する機能と細線・四辺枠は、[SeenKid/shavit-line](https://github.com/SeenKid/shavit-line) を参考にしています。作者・貢献者に感謝します。SourcePawnコードの転載ではなく、CS2向けの独立した実装です。参照元と同梱物のライセンスは `THIRD_PARTY.md` を参照してください。

AGPL-3.0。
