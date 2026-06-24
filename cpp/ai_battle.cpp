// ai_battle.cpp — AI戦シミュレーション（P1=DLL, P2〜P5=決定論bot）
//
// コンテストの「AI戦」フェーズを再現する。P2〜P5 は prog.cpp の
// bot_p2〜bot_p5 と同一ロジック。P1 だけ提出DLLが操作する。
// det_solve の効果測定に使う。
//
// ビルド: g++ -O2 -std=c++14 ai_battle.cpp -o ai_battle.exe
// 実行  : ai_battle.exe <dll> [games] [board] [players] [takes] [seed]

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <string>

static const int Y_MAX = 15, X_MAX = 15, LANDS_MAX = 10;
static const int DR[4]={-1,1,0,0}, DC[4]={0,0,-1,1};

typedef struct { int x, y; } TAKE_TAG;
typedef struct { char your_number, number_takes, number_players; } RULES_TAG;
typedef void (*StartFn)();
typedef void (*EndFn)();
typedef void (*PlayFn)(char[Y_MAX][X_MAX], char*, TAKE_TAG[LANDS_MAX], RULES_TAG);

struct G {
    int H, W, n, takes, empty;
    std::vector<int> g;  // -1壁 0空き 1..n
    int at(int r,int c) const { return g[r*W+c]; }
    void set(int r,int c,int v){ g[r*W+c]=v; }
};

// ---- 決定論bot（prog.cpp と同一） ----
static void bot_p2(G& b, int cur) {           // 左上→右下（行優先・上の行から）
    for (int r=0;r<b.H;++r) for (int c=0;c<b.W;++c)
        if (b.at(r,c)==0){ b.set(r,c,cur); --b.empty; return; }
}
static void bot_p3(G& b, int cur) {           // 右下→左上（行優先・下の行から）
    for (int r=b.H-1;r>=0;--r) for (int c=b.W-1;c>=0;--c)
        if (b.at(r,c)==0){ b.set(r,c,cur); --b.empty; return; }
}
static void bot_p4(G& b, int cur) {           // 横最大ラン: 数大→左→上
    int k=b.takes,best=0,bestCol=0,bestRow=0,br=-1,bc=-1;
    for (int r=0;r<b.H;++r){ int c=0;
        while(c<b.W){ if(b.at(r,c)!=0){++c;continue;}
            int s=c; while(c<b.W&&b.at(r,c)==0)++c; int take=std::min(c-s,k);
            bool better=(br<0)||take>best||(take==best&&s<bestCol)||(take==best&&s==bestCol&&r<bestRow);
            if(better){best=take;bestCol=s;bestRow=r;br=r;bc=s;} } }
    if(br>=0) for (int i=0;i<best;++i){ b.set(br,bc+i,cur); --b.empty; }
}
static void bot_p5(G& b, int cur) {           // 縦最大ラン: 数大→上→左
    int k=b.takes,best=0,bestRow=0,bestCol=0,br=-1,bc=-1;
    for (int c=0;c<b.W;++c){ int r=0;
        while(r<b.H){ if(b.at(r,c)!=0){++r;continue;}
            int s=r; while(r<b.H&&b.at(r,c)==0)++r; int take=std::min(r-s,k);
            bool better=(br<0)||take>best||(take==best&&s<bestRow)||(take==best&&s==bestRow&&c<bestCol);
            if(better){best=take;bestRow=s;bestCol=c;br=s;bc=c;} } }
    if(br>=0) for (int i=0;i<best;++i){ b.set(br+i,bc,cur); --b.empty; }
}
static void apply_bot(G& b, int cur){
    if (cur==2) bot_p2(b,cur); else if (cur==3) bot_p3(b,cur);
    else if (cur==4) bot_p4(b,cur); else bot_p5(b,cur);
}

static bool valid(const G& b, const std::vector<int>& cells){
    int cnt=(int)cells.size(); if(cnt<1||cnt>b.takes) return false;
    for(int v:cells){ int r=v/b.W,c=v%b.W;
        if(r<0||r>=b.H||c<0||c>=b.W) return false;
        if(b.at(r,c)!=0) return false; }
    std::vector<int> seen={cells[0]}, stk={cells[0]};
    auto inset=[&](int v){ for(int x:cells) if(x==v) return true; return false; };
    auto seenq=[&](int v){ for(int x:seen) if(x==v) return true; return false; };
    while(!stk.empty()){ int v=stk.back();stk.pop_back(); int r=v/b.W,c=v%b.W;
        for(int d=0;d<4;++d){ int nr=r+DR[d],nc=c+DC[d];
            if(nr<0||nr>=b.H||nc<0||nc>=b.W) continue; int nv=nr*b.W+nc;
            if(inset(nv)&&!seenq(nv)){ seen.push_back(nv); stk.push_back(nv);} } }
    return (int)seen.size()==cnt;
}

int main(int argc,char**argv){
    if(argc<2){ printf("使い方: ai_battle <dll> [games=30] [board=11] [players=5] [takes=3] [seed=1]\n"); return 1; }
    std::string path=argv[1];
    int games  =(argc>2)?atoi(argv[2]):30;
    int board  =(argc>3)?atoi(argv[3]):11;
    int players=(argc>4)?atoi(argv[4]):5;
    int takes  =(argc>5)?atoi(argv[5]):3;
    unsigned seed=(argc>6)?atoi(argv[6]):1;

    int wins=0, losses=0, invalid=0;
    std::mt19937 rng(seed);

    printf("=== AI戦シミュレーション ===\nDLL=%s  games=%d board=%dx%d players=%d takes=%d\n\n",
           argv[1],games,board,board,players,takes);

    // DLL は1回だけロード（毎ゲーム再読込するとdetachedスレッド解放でクラッシュ）。
    // 終盤ソルバーは独自の時間制限を持つので、この方式でもsolver比較は妥当。
    HMODULE h=LoadLibraryA(path.c_str());
    if(!h){ printf("LoadLibrary失敗 err=%lu\n",GetLastError()); return 1; }
    StartFn S=(StartFn)GetProcAddress(h,"StartStage");
    EndFn   E=(EndFn)  GetProcAddress(h,"EndStage");
    PlayFn  P=(PlayFn) GetProcAddress(h,"PlayStage");

    for(int gi=0;gi<games;++gi){
        S();  // StartStage: モード検出リセット

        G b; b.H=board;b.W=board;b.n=players;b.takes=takes;
        b.g.assign(board*board,0); b.empty=board*board;
        int walls=(int)(board*board*0.10);
        std::uniform_int_distribution<int> di(0,board*board-1);
        for(int i=0;i<walls;++i){ int p=di(rng); if(b.g[p]==0){b.g[p]=-1;--b.empty;} }

        int cur=1, winner=0, guard=0;
        while(b.empty>0 && guard++<100000){
            if(cur==1){
                char floor[Y_MAX][X_MAX];
                for(int r=0;r<Y_MAX;++r)for(int c=0;c<X_MAX;++c)
                    floor[r][c]=(r<b.H&&c<b.W)?(char)b.g[r*b.W+c]:(char)-1;
                char count=0; TAKE_TAG lands[LANDS_MAX]; memset(lands,0,sizeof(lands));
                RULES_TAG rl; rl.your_number=1; rl.number_takes=(char)takes; rl.number_players=(char)players;
                P(floor,&count,lands,rl);
                std::vector<int> cells;
                for(int i=0;i<(int)count&&i<LANDS_MAX;++i) cells.push_back(lands[i].y*b.W+lands[i].x);
                if(!valid(b,cells)){ winner=-1; break; }  // P1不正手→負け
                for(int v:cells){ b.g[v]=1; --b.empty; }
            } else {
                apply_bot(b,cur);
            }
            if(b.empty==0){ winner=cur; break; }
            cur=cur%b.n+1;
        }
        E();

        if(winner==1) ++wins;
        else if(winner==-1){ ++invalid; ++losses; }
        else ++losses;
        printf("game %2d: %s\n", gi+1,
               winner==1?"WIN":(winner==-1?"INVALID(lose)":"lose"));
    }

    printf("\n=== 結果 ===\nP1勝利: %d / %d  (%.1f%%)\n", wins, games, 100.0*wins/games);
    if(invalid) printf("不正手: %d\n", invalid);
    fflush(stdout);
    return 0;
}
