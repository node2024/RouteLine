# RouteLine Workshop描画

Linux版RouteLineと、別配布の `routeline-0.16.19-workshop.zip` を使います。このZIPでは配置先をユーザーが入力します。素材の内容はv0.16.18と同じです。標準プロファイルは `workshop-flat` です。現在のCS2KZ設定を使ってGlobalやSRを選びます。

## v0.16.18への更新

Linux版の `routeline.so` と、既存の `routeline` Workshopアイテムを更新してください。マップを再読み込みし、`!line status` に `Marker renderer: mesh` と出れば、新しい平面モデルで描画しています。これまでの粒子素材も同梱しているため、旧Linux版や旧素材との組み合わせでは従来の描画を使います。

## v0.16.13への更新

Linux版の `routeline.so` を差し替え、同梱バッチで素材を配置してWorkshop Managerで既存の `routeline` アイテムを更新します。サーバーとクライアントが更新を取得した後、マップを再読み込みします。旧素材のままでもルート線と接地枠は表示でき、×印だけが無効になります。

## v0.16.5の素材

v0.16.4からはWorkshop素材の更新だけで適用できます。v0.16.3以前からはLinux版の `routeline.so` も更新します。同梱バッチで素材を配置し、Workshop Managerで `routeline` の既存アイテムを更新してください。サーバーで `mm_download_addon <アイテムID>` を実行し、取得後にマップを再読み込みします。クライアントでSteamの更新がファイルロックにより止まった場合は、CS2を終了して更新を完了させてから再接続します。

ルート線と接地枠はアルファ合成と等倍の明るさで描画します。暗い場所でも素材の色を出すため、拡散照明は無効です。

平面枠の既定色は `marker_flat_blue.vpcf` と `marker_flat_red.vpcf` に直接入れています。設定でRGBを変えた場合は、色データを受け取る `marker_flat.vpcf` を使います。マップ起動後に `!line preview` で青・赤・青の3枠を確認できます。

`render.path_batch_frames` で線をまとめるフレーム数を指定できます。標準は144です。`reload_admins` に登録した管理者は `!line reload` で設定を反映できます。初回の登録方法は [README.md](README.md) を参照してください。


標準の `workshop-flat` はルート線の粒子間隔20、Follow中の表示距離はラインが1,200ユニット、平面のジャンプ印と接地枠が600ユニットです。`!line status` で両方の距離と描画数を確認できます。

同色の連続パスの最大長を1280に広げました。`render.path_batch_length` を640にすると従来の分割に戻ります。粒子素材は変更していません。

ロープ式の枠を使うプロファイルは従来のWorkshop素材で動作します。平面枠では、追加した `marker_flat.vpcf` と `marker_flat.vtex` を使います。着地枠の中心はどちらも同じ位置補正を通り、通常接地は青、しゃがみ接地は赤です。

`!line render all` と `!line render follow` で表示範囲を切り替えます。`!line status` の `Display` と `Paths` で方式とパス数を確認できます。全体表示がサーバーの描画枠に収まらない場合は、Followを選んで読み込み直せます。

接地枠は20×20の水平な四角です。中心は記録された接地点付近の足場へ補正します。補正できた中心は足場の上2.5ユニット、見つからなければ記録位置＋ `z_offset` です。枠の空中へのはみ出しを許容します。`landing_width` と `marker_particle_spacing` はロープ式に適用します。`compact` / `landings` モードは短線を表示します。

広い床と狭い足場で `!line preview` と実リプレイを確認してください。

## 素材の導入

Followは既定で最大3,000フレームを表示し、1,500フレーム進むごとに次の範囲へ切り替えます。重なる範囲のパスを再利用し、新しいパスは既定で1サーバーフレームあたり最大4個ずつ生成します。

v0.11.1ではルート線の粒子間隔を4から16ユニット、着地枠を1から4ユニットに変更し、ルート素材の追加分割を最大2から1に減らしました。既存サーバーもLinux版とWorkshop素材の両方を更新してください。素材は同梱バッチで配置したあと、Workshop Managerで既存アイテムを更新し、サーバーとクライアントで更新を取得してマップを再読み込みします。

`!line status` にルート線の粒子間隔が表示されます。使用中プロファイルの `render.route_particle_spacing` を変更したら、管理者は `!line reload`、サーバーコンソールでは `routeline_reload` で反映できます。

1. Linux用ZIPからRouteLineを導入します。既存の `addons/routeline/config.json` から移行する場合は、`workshop-flat` を `active_profile` に設定します。
2. Workshop用ZIPをWindows PCで全展開し、`Install-RouteLine-Addon.bat` をダブルクリックします。表示される入力欄に、配置先の `Counter-Strike Global Offensive` フォルダのパスを入力してください。指定したフォルダの `game/` と `content/` の `csgo_addons/routeline/` に素材を配置します。変更する既存素材は展開先の `backup-...` に保存し、既存の `addoninfo.txt` はそのまま使います。
3. Counter-Strike 2 Workshop Managerで `routeline` アドオンを選び、公開してアイテムIDを控えます。アドオンにはルートと着地枠の素材が入っています。
4. [MultiAddonManager](https://github.com/Source2ZE/MultiAddonManager/blob/main/README.md) の `mm_extra_addons` にアイテムIDを追加し、マップを再読み込みします。既存のアドオンIDがあればカンマ区切りで追加します。RouteLineはマップ開始時に素材を登録するため、マップ起動前からロードしておきます。
5. 広い床の上で `!line preview` を試し、青・赤・青の接地枠と黄橙・緑の×印を確認します。`!line status` には `Contact markers` と `Jump marks` の件数が表示されます。その後 `!line off` を実行し、`!line global pro` または `!line sr pro` で実リプレイを表示します。

Workshop素材がマップに登録されていない場合は、素材をマウントしてマップを再読み込みしてください。

Workshop描画は連続する軌跡を `path_particle_rope` にまとめます。標準方式は色の境界でも分け、packedは白と赤を頂点色で同じパスにまとめます。平面枠は同じ色の着地枠を最大4個ずつ描きます。描画はゲーム内の深度テストを使います。

v0.10.0の素材は線と枠の明るさを上げ、フォグの影響を外しています。着地枠の線幅を広げ、Workshopの既定色を明るい青にしました。v0.9以前の素材で公開済みの場合は、バッチで素材を配置したあと、Workshop Managerで同じアイテムを更新してください。サーバーとクライアントに更新後の素材を取得させ、マップを再読み込みします。

接地枠はリプレイの接地点付近へ位置補正して表示します。枠の四隅が足場に載っている保証はありません。`!line status` の `Contact markers` で生成数と固定20×20の表示方式を確認できます。

素材を再ビルドする場合は、ソースZIPを展開し、WindowsのPowerShellで次を実行します。CS2 Workshop Toolsが必要です。

```powershell
./workshop/Build-Assets.ps1 -Cs2Root 'D:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive'
```
