// test_genmoves.cpp — gen_moves_dfs 再帰版と反復版の同値性チェック
// g++ -O2 -std=c++14 test_genmoves.cpp -o test_genmoves && ./test_genmoves
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>
#include <cstdint>

static const int DR[4]={-1,1,0,0}, DC[4]={0,0,-1,1};
struct Board { int8_t g[15][15]; int H,W,number_takes; };
struct Move { int cells[10]; int n; };

// ---- 旧: 再帰版 ----
static void dfs_rec(const Board& b, Move& cur, int anchor,
                    std::vector<Move>& out, int max_n, int& budget) {
    if (budget<=0) return;
    out.push_back(cur); --budget;
    if (cur.n>=max_n) return;
    for (int si=0; si<cur.n; ++si) {
        if (budget<=0) return;
        int r0=cur.cells[si]/b.W, c0=cur.cells[si]%b.W;
        for (int d=0; d<4; ++d) {
            int r2=r0+DR[d], c2=c0+DC[d];
            if (r2<0||r2>=b.H||c2<0||c2>=b.W) continue;
            if (b.g[r2][c2]!=0) continue;
            int nb=r2*b.W+c2;
            if (nb<=anchor) continue;
            bool dup=false; for(int i=0;i<cur.n;++i) if(cur.cells[i]==nb){dup=true;break;}
            if (dup) continue;
            cur.cells[cur.n++]=nb;
            dfs_rec(b,cur,anchor,out,max_n,budget);
            cur.n--;
            if (budget<=0) return;
        }
    }
}

// ---- 新: 反復版 ----
static void dfs_iter(const Board& b, const Move& seed, int anchor,
                     std::vector<Move>& out, int max_n, int& budget) {
    std::vector<Move> stack; stack.push_back(seed);
    while (!stack.empty()) {
        if (budget<=0) return;
        Move cur=stack.back(); stack.pop_back();
        out.push_back(cur); --budget;
        if (cur.n>=max_n) continue;
        for (int si=0; si<cur.n; ++si) {
            int r0=cur.cells[si]/b.W, c0=cur.cells[si]%b.W;
            for (int d=0; d<4; ++d) {
                int r2=r0+DR[d], c2=c0+DC[d];
                if (r2<0||r2>=b.H||c2<0||c2>=b.W) continue;
                if (b.g[r2][c2]!=0) continue;
                int nb=r2*b.W+c2;
                if (nb<=anchor) continue;
                bool dup=false; for(int i=0;i<cur.n;++i) if(cur.cells[i]==nb){dup=true;break;}
                if (dup) continue;
                Move nx=cur; nx.cells[nx.n++]=nb;
                stack.push_back(nx);
            }
        }
    }
}

// dedup して正準形のリストにする（budget 上限なしで全列挙）
static std::vector<std::vector<int>> canon(const Board& b, bool useIter) {
    std::vector<Move> out;
    int k = (b.number_takes>0)? std::min(b.number_takes,10):3;
    for (int r=0;r<b.H;++r) for (int c=0;c<b.W;++c) {
        if (b.g[r][c]!=0) continue;
        Move m{}; m.n=1; m.cells[0]=r*b.W+c;
        int budget=100000000;  // 実質無制限で全列挙を比較
        if (useIter) dfs_iter(b,m,m.cells[0],out,k,budget);
        else         dfs_rec(b,m,m.cells[0],out,k,budget);
    }
    std::vector<std::vector<int>> res;
    for (Move& m: out){ std::vector<int> v(m.cells,m.cells+m.n); std::sort(v.begin(),v.end()); res.push_back(v); }
    std::sort(res.begin(),res.end());
    res.erase(std::unique(res.begin(),res.end()),res.end());
    return res;
}

int main(){
    std::mt19937 rng(7);
    int fails=0, total=0;
    for (int t=0;t<2000;++t){
        Board b; b.H=3+rng()%8; b.W=3+rng()%8; b.number_takes=1+rng()%5;
        for(int r=0;r<b.H;++r)for(int c=0;c<b.W;++c) b.g[r][c]=(rng()%100<20)?-1:0;
        auto a=canon(b,false), c2=canon(b,true);
        ++total;
        if (a!=c2){ ++fails;
            if (fails<=3) printf("MISMATCH H=%d W=%d k=%d  rec=%zu iter=%zu\n",
                                 b.H,b.W,b.number_takes,a.size(),c2.size());
        }
    }
    printf("%d/%d 一致%s\n", total-fails, total, fails? " ★不一致あり":" 完全一致 ✓");
    return fails?1:0;
}
