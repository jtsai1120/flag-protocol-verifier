# Flag protocol verifier

這個工具判斷一個 flag fault-tolerant 協定是否真的容錯。

輸入是用 FPDL 寫的協定描述，以及它引用的 QASM 電路。驗證時會：

- 枚舉所有可能的執行路徑；
- 在每個 fault location 注入所有可能的 fault；
- 檢查是否存在一條路徑，使得兩個無法區分的 error 相乘之後落在 `N(S)\S`。若存在，decoder 修正其中一個時就會傷到另一個，協定不容錯。

Pauli 集合的表示與運算全部交給 [SPBDD](https://github.com/fmlab-iis/spbdd)。SPBDD 以 BuDDy 的 BDD 表示多量子位元 Pauli 集合。

```
.fpdl ──► fpdl::Parser ──┬─► code: [[n,k,d]] + generators ──► tau = ⌊(d−1)/2⌋
                          ├─► SE 宣告（qd/qm/qf, file）
                          └─► SymbolicPaths ──► ftec::build_dag ──► trie
                                                                     │
                                                          DFS + 條件路由
                                                                     ▼
                                                           spbdd backend
                                              （狀態傳遞、fault 注入、測量分裂、
                                                最終 E1E2 查詢）
```

## 建置

需要 `g++`（C++20）、`make` 與 `git`。

```bash
make          # 抓 SPBDD → 建出 libspbdd.a → 建 build/ftec-verify
make check    # 另外建並跑測試
```

`make` 的流程如下：

1. 把 `fmlab-iis/spbdd` 抓到 `third_party/spbdd-src`，版本釘在 `Makefile` 裡的 `SPBDD_REF`。
2. 用 SPBDD 自己的 Makefile 建出 `libspbdd.a`。這一步會抓 BuDDy，並把 BuDDy 一起包進同一個 archive。
3. 只保留 archive 與公開標頭：
   ```
   third_party/spbdd/lib/libspbdd.a
   third_party/spbdd/include/spbdd/{spbdd,code,context,pset}.hpp, bdd.h
   ```
4. 驗證器本身只連結這個 archive。

| 指令 | 用途 |
|---|---|
| `make SPBDD_REF=<branch/tag/commit>` | 換一個 SPBDD 版本（先 `make distclean`） |
| `make SPBDD_SRC=/path/to/spbdd` | 直接用手邊的 SPBDD checkout，不抓 |
| `make clean` | 刪 `build/` |
| `make distclean` | 連 `third_party/` 一起刪 |

## 使用

```bash
./build/ftec-verify <protocol.fpdl> [選項]

./build/ftec-verify "protocols/CR17_[[5,1,3]]/CR17_[[5,1,3]].fpdl"
./build/ftec-verify "protocols/CB18_[[17,1,5]]_plain/CB18_[[17,1,5]]_plain.fpdl" --fqcv=table
```

| 選項 | 預設值 | 說明 |
|---|---|---|
| `--fqcv=METHOD` | `image` | 最終 E1E2 查詢的方法，可選 `bruteforce`、`table`、`image`、`square`（見下） |
| `--bound=N` | 倍增搜尋 | 固定 BMC bound；不給時從 16 倍增，直到展開完整（上限 65536） |
| `--max-paths=N` | `5000` | 符號路徑數超過就放棄 |
| `--first` | 關閉 | 遇到第一條不受保護的路徑就停 |
| `--dag` | 關閉 | 只印合併後的路徑結構，不做驗證 |

輸出包含：

- 到達的 paths、records、circuits 數量；
- 判決：`no unprotected path found`，或列出每條失效路徑、它的 fault 數與量測紀錄；
- backend 統計：cache 命中率、fqcv 時間與 BDD 大小；
- 展開時間、走訪時間與峰值記憶體。

BDD 動態變數重排固定關閉，因為在任何夠大、值得計時的協定上，重排都沒有帶來好處。

### 最終 E1E2 的四種方法

四種方法都是 SPBDD 的 `code::find_qec_cond_violation`，回答同一個問題，判決必定相同，差別只在成本：

| 方法 | 做法 |
|---|---|
| `bruteforce` | 列舉 E 的元素，兩兩相乘檢查 |
| `table` | 列舉 E 一次，以 syndrome 為鍵建表 |
| `image` | 沿 E 的 BDD 層做動態規劃，計算 label 集合的像 |
| `square` | 算 E·E，問它與 N(S)−S 是否相交（N(S)−S 每個 code 只建一次） |

### 批次量測

`tools/bench_fqcv.py` 對每個協定、每種方法各跑一個獨立行程，所以峰值記憶體是該方法自己的。結果可輸出 JSON、CSV 或 Excel。統計把「最終 E1E2」和「之前的所有過程」分開，計算方式為 `E1E2 前 = traversal − fqcv − diagnostics`。

```bash
tools/bench_fqcv.py --xlsx fqcv.xlsx
tools/bench_fqcv.py --protocol 25,1,5 --methods table,image,square --timeout 2000 --json out.json
```

### 產生更高 qubit 的 CB18 協定

`tools/gen_cb18_surface.py` 依 Chamberland & Beverland, *Flag fault-tolerant error correction with arbitrary distance codes*（Quantum 2, 53, 2018）第 3 節的通用 t-FTEC 演算法，為 rotated surface code 產生 FPDL 與 `FSE_f` / `FSE_b` 電路。

- weight-4 的 check 使用 Fig. 2(b) 的單 flag 電路；weight-2 的 check 不加 flag。
- 可以產生 d×d 的方形 lattice，也可以產生 d×C 的長方形 lattice（距離 d，qubit 數為 d·C）。
- 產生的 d=5 協定與手寫的 `CB_18[[25,1,5]]_plain` 逐字相同。

```bash
tools/gen_cb18_surface.py 5 --out protocols            # [[25,1,5]]
tools/gen_cb18_surface.py 5 --cols 9 --out protocols   # [[45,1,5]]，5×9
tools/gen_cb18_surface.py 7 --out protocols            # [[49,1,7]]
```

參考量測：2 vCPU、7.8 GiB，兩個行程並行。

| protocol | E1E2 前 (s) | table fqcv | image fqcv | square fqcv | 峰值記憶體 |
|---|---|---|---|---|---|
| CB18 [[25,1,5]] | 201–210 | 0.96 s | 1.85 s | 1.65 s | 157 MiB |
| CB18 [[35,1,5]] 5×7 | 741–762 | 3.22 s | 4.45 s | 4.90 s | 331 MiB |
| CB18 [[45,1,5]] 5×9 | 2215–2238 | 7.81 s | 10.47 s | 12.91 s | 601 MiB |
| CB18 [[49,1,7]] | 超過 2000 s 未完成 | | | | |

瓶頸在 E1E2 前的狀態傳遞與 fault 注入；最終 E1E2 只占總時間約 0.5%。

## 測試

`make check` 會跑三組測試：

- `parser_tests`：解析 `protocols/` 下所有 FPDL。
- `qasm_tests`：解析所有 QASM 電路。
- `fqcv_tests`：對所有 distance-3 協定，用四種方法各驗證一次。四種方法必須彼此一致，也必須等於 `tests/expected.txt` 記錄的判決。`expected.txt` 建立時，已和另一個獨立手寫的 BuDDy backend 交叉比對過。

## 檔案結構

```
Makefile
include/fpdl/parser.hpp      FPDL 前端
include/ftec/                驗證核心：qasm、dag（路徑 trie）、verify（走訪）、
                             backend（介面）、spbdd_backend
src/                         對應實作；ftec_verify_main.cpp 是 CLI
tests/                       parser、qasm、fqcv 測試與 expected.txt
tools/bench_fqcv.py          批次量測
tools/gen_cb18_surface.py    CB18 協定產生器
protocols/                   協定測資，一個目錄一個協定（.fpdl + 它引用的 .qasm）
docs/FPDL.md                 FPDL 語言說明
```

## 已知限制

- `Outcome` 把 syndrome 打包成 `uint64`，所以 syndrome 超過 64 bit 的 code（超過 65 qubit 的 rotated surface code）需要先修改這一點。
- 預設 `--max-paths=5000`。
