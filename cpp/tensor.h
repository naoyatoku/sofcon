// ===================================================================== //
//  tensor.h  —  conv2d 用テンソル・アクセスヘルパー（WIP / レビュー中）
//
//  方針:
//   - 配列実体は持たない。先頭ポインタ + offset で「窓」をずらして使い回す。
//   - 範囲外アクセスは 0 を返す → conv の padding をこれで自然に表現する。
//   - dot() の x_ofs / y_ofs で、カーネルを入力上でスライドさせる。
//
//  ⚠ レビュー指摘（2026-06-28、別PCでの続き用メモ）:
//   1) 🔴致命: tensor_2d::v() の添字が `y*__W*x` になっている → 正しくは
//      `y*__W + x`（row-major）。下の FIXME 参照。まずここを直す。
//   2) 🟠 `__name`（先頭2アンダースコア）は C++ 標準で処理系予約 → 改名推奨
//      (m_parr, w_, h_, d_ など)。
//   3) 🟠 tensor_3d::__win_cache は共有可変状態でスレッド非安全。推論は
//      スレッドプール並列なので、カーネル重みを共有するなら dot() 内で
//      ローカルな tensor_2d を作る方式に変える（v() と同方式）。
//   4) 🟡 dot() は形状 ASSERT 無し / 毎要素 範囲チェック分岐あり。正しさ優先で
//      動かしてから、内部は分岐除去で高速化（全50ステージ30秒制限のため）。
//
//  TODO（明日の続き）:
//   - 上記1)を修正 → 既存 inference_impl.h の conv2d と同じ入力で出力一致を確認
//     する単体テストを書く（cpp/test_tensor.cpp 想定）。
//   - ASSERT マクロ依存（SOFCON_DEBUG 時のみ有効な自前マクロを用意すること）。
// ===================================================================== //
#pragma once

class tensor
{
private:
    const float   * __p_arr;      //toku　これは、tensor_2dが並んでいるとみなします。
    int             __alt;        //先頭アドレスを変化させられるためのオフセット値です。（位置ずらすときに、いちいち新しいオブジェクトを作らなくていいように）
    int             __size;
protected:
    //各テンソルのサイズ分のオフセットをさせる。(戻り値はその時の左上アドレス)
    void     _alt(int n){
        __alt = n * __size;
    }

public:
    tensor(const float* p_arr, int sz) : __p_arr(p_arr), __alt(0), __size(sz) {}
    //基準ポイントをそれぞれ計算したい場所に応じて変化させられるように
    //各テンソルの左上アドレスを返す
    const float *p_head()const{
        return __p_arr + __alt;
    }
    int size()const { return __size; }
};

class tensor_2d : public tensor
{
    //shape
    const int __W;
    const int __H;
protected:
public:
    tensor_2d(const float* p_arr, int h, int w) : tensor(p_arr, h* w), __H(h), __W(w) {}

    //この二次元テンソルが並んでいるとして、n面目の先頭アドレスを返すようにします。
    //  float *p_head_n(int n)const {   return  __p_arr + (n*size());   }    //とりあえず、
        //※convのpadding対応のために、ofsをもった場所を指示できるようにします。
    float dot(const tensor_2d& a, int x_ofs = 0, int y_ofs = 0)const {
        float ret = 0.0;
        //dot積の範囲は、aに合わせます。
        for (int y = 0; y < a.H(); ++y) {
            for (int x = 0; x < a.W(); ++x) {                   //aの範囲を全部、
                ret += v(x + x_ofs, y + y_ofs) * a.v(x, y);             //自分の範囲は、ofs分ずらした位置
            }
        }
        return ret;
    }
    //先頭位置をずらす
    tensor_2d& alt(int n) { tensor::_alt(n);        return *this; }

    //このhsapeの配列へアクセスするような値を返す関数。[][]のような
    float v(int x, int y)const {        //p_headは好きな場所を指示する場合に使います。
        if (x < 0 || x >= __W || y < 0 || y >= __H) {
            return 0;                   //範囲外は0とします。
        }
        // FIXME(🔴): `y * __W * x` は誤り。row-major なので `y * __W + x` が正しい。
        return *(p_head() + (y * __W * x));
        //                  ^^^^^^^^^^^
        //                  この平面がどこのオフセットかを自由に設定できる。（保証は呼び出し側で）
    }
    int H()const { return __H; }
    int W()const { return __W; }
};

//配列実体は内包しないで、アクセスヘルパーです。
class tensor_3d :public tensor
{
    const int   __D;          //これが奥行。
    const int   __H;
    const int   __W;          //
    //高速化のため、2dテンソルのウィンドウキャッシュです。
    tensor_2d   __win_cache;        //
protected:
public:
    //3dテンソルが並んでいるとして、n番目の先頭アドレスを返す。
//  float *p_headn_n(int n){      }
    //この3dテンソルは、
    //      A():    9×15*15
    //      K():    9*3*3
    //paddingの処理も必要。
    tensor_3d(float *p_arr , int d , int h , int w) : tensor(p_arr,d*h*w),__D(d),__H(h),__W(w),__win_cache(p_arr,h,w) {}

    //aの深さに合わせて、dotをとります。
    float dot( tensor_3d& a,int x_ofs=0,int y_ofs=0){
        float ret=0.0;
        for(int z = 0 ; z < a.D() ; ++z ){      //このZレイヤーの面のdot積を考える。aは左上から、スタート、自分自身は、どこの部分を
            //このテンソルのzの先頭
            ret += win(z).dot(a.win(z), x_ofs, y_ofs);
        }
        return ret;
    }
    //先頭位置をずらす
    tensor_3d &alt(int n){        tensor::_alt(n);  return *this;   }
    //このテンソルが並んでいるとしてidx番目の3dテンソルなかの値を取り出す。
    float v(int x,int y ,int z)const{
        //範囲外はここで
        if(z < 0 || z >= __D ){
            return 0.0;
        }
        tensor_2d window(p_head(), __H, __W);
        return window.alt(z).v(x,y);
    }
    tensor_2d& win(int z) {     //z指示したときの2dテンソルウィンドウを返す。(not thread safe)
        ASSERT(z >= 0 && z < __D, "tensor_3d::win : overflow");
        return __win_cache.alt(z);
    }
    int D()const { return __D; }
    int H()const { return __H; }
    int W()const { return __W; }
};
