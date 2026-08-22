// P1a 行为等价测试：镜像 rtl/debug/sf_micro_ila.sv 的 RTL 语义，
// 验证环形采集、掩码触发、写满停止与宿主端重排算法。
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& message)
{
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

// 与 RTL 一一对应的行为模型。
struct MicroIlaModel {
    int depth = 64;
    int width = 32;
    std::vector<std::uint64_t> mem;

    std::uint64_t wptr = 0;
    bool running = false;
    bool done = false;
    bool triggered = false;
    std::uint64_t triggerIndex = 0;

    std::uint64_t probe = 0;
    std::uint64_t mask = 0;
    std::uint64_t value = 0;
    bool arm = false;
    bool rstN = true;

    explicit MicroIlaModel(int d, int w) : depth(d), width(w), mem(d, 0) {}

    std::uint64_t Masked() const { return (std::uint64_t(1) << width) - 1; }

    void Step()
    {
        if (!rstN) {
            running = false;
            done = false;
            triggered = false;
            wptr = 0;
            triggerIndex = 0;
            return;
        }
        if (arm) {
            running = true;
            done = false;
            triggered = false;
            wptr = 0;
            return;
        }
        if (!running) return;

        mem[wptr] = probe & Masked();
        if (!triggered && ((probe & mask & Masked()) == (value & Masked()))) {
            triggered = true;
            triggerIndex = wptr;
        }
        if (wptr == static_cast<std::uint64_t>(depth - 1)) {
            running = false;
            done = true;
        }
        wptr = (wptr + 1) % static_cast<std::uint64_t>(depth);
    }

    std::uint64_t At(std::uint64_t addr) const { return mem[addr % depth]; }
};

void TestTriggerAndReorder()
{
    constexpr int kDepth = 16;
    constexpr int kWidth = 8;
    MicroIlaModel dut(kDepth, kWidth);
    dut.mask = 0xFF;
    dut.value = 0x0A;

    // 复位后 ARM，probe 每周期递增（写入序列与周期一致）
    dut.Step();
    dut.arm = true;
    dut.Step();
    dut.arm = false;
    Check(dut.running, "busy after arm");

    for (int cycle = 0; cycle < kDepth + 4; ++cycle) {
        dut.Step();
        dut.probe = (dut.probe + 1) & 0xFF;
    }
    Check(dut.done, "done after DEPTH samples");
    Check(!dut.running, "not running when done");
    Check(dut.triggered, "triggered observed");
    Check(dut.triggerIndex == 10, "trigger_index == 10");

    // 写入序列：周期 c 写入值 c 到地址 c → mem[a] == a
    // 宿主重排：rel k = mem[(ti + k) % DEPTH]
    Check(dut.At(dut.triggerIndex) == 0x0A, "rel 0 is trigger sample");
    Check(dut.At(dut.triggerIndex + 1) == 11, "rel +1");
    Check(dut.At(dut.triggerIndex - 1) == 9, "rel -1");
    for (int k = 0; k < kDepth; ++k) {
        const std::uint64_t rel =
            dut.At((dut.triggerIndex + static_cast<std::uint64_t>(k)) % kDepth);
        Check(rel == static_cast<std::uint64_t>((dut.triggerIndex + k) % kDepth),
              "reorder round trip k=" + std::to_string(k));
    }
    Check(dut.At(dut.triggerIndex - 3) == 7, "rel -3 (wrap)");
}

void TestImmediateTrigger()
{
    MicroIlaModel dut(16, 8);
    dut.mask = 0; // 立即触发
    dut.value = 0;
    dut.Step();
    dut.arm = true;
    dut.Step();
    dut.arm = false;
    dut.Step(); // 首个采样周期：写入 probe=0 并触发
    Check(dut.triggered, "immediate trigger");
    Check(dut.triggerIndex == 0, "immediate trigger index 0");
}

void TestNoTriggerFullDepth()
{
    MicroIlaModel dut(64, 8);
    dut.mask = 0xFF;
    dut.value = 0xFE; // 0..63 内不会命中
    dut.Step();
    dut.arm = true;
    dut.Step();
    dut.arm = false;
    int steps = 0;
    while (!dut.done && steps < 200) {
        dut.Step();
        dut.probe = (dut.probe + 1) & 0xFF;
        ++steps;
    }
    Check(dut.done, "done without trigger");
    Check(!dut.triggered, "not triggered");
    Check(steps == 64, "exactly DEPTH samples captured");
}

void TestReset()
{
    MicroIlaModel dut(16, 8);
    dut.Step();
    dut.arm = true;
    dut.Step();
    dut.arm = false;
    dut.rstN = false;
    dut.Step();
    Check(!dut.running && !dut.done && !dut.triggered, "reset clears state");
}

} // namespace

int main()
{
    TestTriggerAndReorder();
    TestImmediateTrigger();
    TestNoTriggerFullDepth();
    TestReset();

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cout << g_failures << " FAILURES\n";
    return 1;
}
