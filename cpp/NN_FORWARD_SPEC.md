# 自作 forward 実装仕様（cpp/tensor.h でNN推論を書くための完全仕様）

目的: `cpp/tensor.h` のテンソルクラスで SofconNet の forward を自作し、
`golden/` の正解値と層ごとに一致させる。一致したら prog.cpp に組み込む。

アーキ: **15×15 / 入力9ch / trunk 16ch / ResBlock×3 / value=6席ベクトル**
（`model/network.py` と一致。`channels=16, num_blocks=3`）

---

## 0. 重みの入手とレイアウト（weights.h）

`cpp/weights.h` に全テンソルが `static const float w_***[]` で入っている。
各テンソルには **`w_***_shape[]`** という形状配列も併設（次元を実行時に確認可）。

- **conv weight**: shape `[C_out, C_in, kH, kW]`、row-major。
  index = `((oc*C_in + ic)*kH + kh)*kW + kw`
- **BN** (`weight=γ, bias=β, running_mean, running_var`): 各 `[C]`
- **linear weight**: shape `[out_features, in_features]`、row-major。
  index = `j*in_features + i`（j=出力, i=入力）。bias は `[out_features]`

`BN_EPS = 1e-5f`（PyTorch 既定）。

---

## 1. 層の順序（これを上から実装する）

入力 `x`: (9, 15, 15)  ※ board_to_tensor と同じ並び（下記§4）

```
[Stem]
  conv(x, w_stem_0_weight, C_in=9→16, 3x3, pad=1)         → t   (16,15,15)
  BN(t, w_stem_1_{weight,bias,running_mean,running_var})  → t
  ReLU(t)                                                 → cur  ★golden: stem

[ResBlock i = 0,1,2]   ※ w_blocks_<i>_*
  r = cur
  conv1(cur, w_blocks_i_conv1_weight, 16→16, 3x3, pad=1)  → t
  BN1 (t, w_blocks_i_bn1_*)                               → t
  ReLU(t)
  conv2(t,  w_blocks_i_conv2_weight, 16→16, 3x3, pad=1)   → t
  BN2 (t, w_blocks_i_bn2_*)                               → t
  t = t + r ; ReLU(t)                                     → cur  ★golden: block{i}

[Policy head]
  conv(cur, w_policy_conv_weight, 16→2, 1x1, pad=0)       → p   (2,15,15)
  BN(p, w_policy_bn_*) ; ReLU(p)
  flatten p → (2*225=450)
  linear(p, w_policy_fc_weight[225,450], w_policy_fc_bias)→ logits (225)
  softmax(logits)                                         → ★golden: policy_probs (225)

[Value head]
  conv(cur, w_value_conv_weight, 16→1, 1x1, pad=0)        → v   (1,15,15)
  BN(v, w_value_bn_*) ; ReLU(v)
  flatten v → (225)
  linear(v, w_value_fc1_weight[256,225], bias) ; ReLU     → (256)
  linear(., w_value_fc2_weight[6,256], w_value_fc2_bias)  → (6)   ★golden: value_vec
  sigmoid(each)。prog.cpp が使うのは index0 = 現手番の勝率 → ★golden: value0_sigmoid
```

★印 = `golden/<name>.bin` に正解がある段。ここで一致を確認する。

---

## 2. 各プリミティブの定義（PyTorch と一致させる要点）

- **conv2d**: `out[oc,oh,ow] = b[oc] + Σ_{ic,kh,kw} in[ic, oh-pad+kh, ow-pad+kw]·w[oc,ic,kh,kw]`
  範囲外は 0（あなたの `tensor::v()` の範囲外0返しがそのまま padding になる）。
  - 3x3 conv は **pad=1**（同サイズ出力）、1x1 conv は **pad=0**。
- **BN(推論)**: `scale=γ/√(var+eps)`, `shift=β−mean·scale`, `out=in·scale+shift`（チャンネル毎）
- **ReLU**: `max(0,x)`
- **residual**: `ReLU(conv-bn結果 + ブロック入力)`（加算してからReLU）
- **linear**: `out[j]=b[j]+Σ_i in[i]·w[j*in+i]`
- **softmax**: max引き安定化版（policy のみ）
- **value**: 各要素 `sigmoid(x)=1/(1+e^-x)`。**tanh ではない**（network.py は sigmoid）。

⚠ 既存 `cpp/inference.h` の宣言コメントは旧版（64ch/5block/tanh）の記述が残るが、
**実装 `inference_impl.h` と network.py が正**。sigmoid・16ch・3block で実装すること。

---

## 3. golden/*.bin のフォーマット（比較ハーネス用）

各ファイル先頭にヘッダ → 本体 float32（C順）:
```
int32  ndim
int32  shape[0..ndim-1]
float32 data[...]   // 全要素 row-major
```
`golden/summary.txt` に各段の L2ノルムと先頭8値、`manifest.txt` に名前と形状。
まず summary の L2 と first8 を目視で合わせ、合えば全要素 diff（許容 1e-4）へ。

再生成: `python tools/dump_golden.py --checkpoint model/checkpoint_16ch_15x15.pt --seed 42`

---

## 4. 入力テンソルの並び（board_to_tensor と一致必須）

(C_in=9, H, W)。現手番から見た相対視点:
```
ch0      : 壁マスク (壁=1)
ch1      : 空きマスク (空き=1)
ch2+k    : k手後に動くプレイヤーの占有 (k=0..5、k=0=現手番)。存在しない席は全0
ch8      : (num_players-1)/(MAX_PLAYERS-1) を全マスに敷く
```
prog.cpp 側に既に同等の入力生成があるはずなので、そこを流用すれば一致する。

---

## 5. 進め方（最短デバッグ手順）

1. `tensor_2d::v()` の `y*__W*x` → **`y*__W + x`** を直す（最初に！）。
2. conv2d を1つ書く → **stem だけ**実装して `golden/stem.bin` と一致確認。
3. ResBlock を1つ書く → `block0` 一致 → ループで block1/2。
4. policy head → `policy_probs` 一致。value head → `value_vec`/`value0_sigmoid` 一致。
5. 全段一致したら prog.cpp の推論呼び出しを自作 forward に差し替え、
   `prog_submit.cpp` を bundle → ビルド → 実機。
6. ASSERT は `SOFCON_DEBUG` 時のみ有効な自前マクロに（本番で abort させない）。

時間が尽きたら: 既存 `inference_impl.h` forward に戻せば確実に動く版で提出できる
（=保険。あなたの自作 forward は差し替え方式なので、最悪そこだけ戻せる）。
