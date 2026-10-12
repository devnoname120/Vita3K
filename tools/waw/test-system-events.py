from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
bodies = []
for path, name in (
    ("SceAppMgr/SceAppMgr.cpp", "_sceAppMgrReceiveSystemEvent"),
    ("SceDriverUser/SceAppMgrUser.cpp", "sceAppMgrReceiveSystemEvent"),
):
    source = (ROOT / "vita3k/modules" / path).read_text()
    signature = f"EXPORT(int, {name}, SceAppMgrSystemEvent *systemEvent)"
    start = source.index(signature)
    end = source.index("\nEXPORT(", start + 1)
    bodies.append(source[start:end].replace(signature, f"int {name}(SceAppMgrSystemEvent *systemEvent)"))

prelude = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#define TRACY_FUNC(...) ((void)0)
#define STUBBED(...) ((void)0)
#define UNIMPLEMENTED() 0
#define RET_ERROR(x) (x)
constexpr int SCE_APPMGR_ERROR_INVALID_PARAMETER = -1;
constexpr int SCE_APPMGR_SYSTEMEVENT_ON_RESUME = 0x10000003;
struct SceAppMgrSystemEvent { int32_t systemEvent; uint8_t reserved[60]; };
"""
tests = r"""
int main() {
    using Receive = int (*)(SceAppMgrSystemEvent *);
    const Receive implementations[] = {_sceAppMgrReceiveSystemEvent, sceAppMgrReceiveSystemEvent};
    for (Receive receive : implementations) {
        struct Guarded { uint64_t before; SceAppMgrSystemEvent event; uint64_t after; } data;
        for (unsigned i = 0; i < 1000; ++i) {
            std::memset(&data, 0xa5, sizeof(data));
            assert(receive(&data.event) == 0);
            const SceAppMgrSystemEvent empty{};
            assert(std::memcmp(&data.event, &empty, sizeof(empty)) == 0);
            assert(data.before == UINT64_C(0xa5a5a5a5a5a5a5a5));
            assert(data.after == UINT64_C(0xa5a5a5a5a5a5a5a5));
        }
        assert(receive(nullptr) == SCE_APPMGR_ERROR_INVALID_PARAMETER);
    }
    std::puts("Both system-event exports pass 1000 idle polls, output guards and null checks");
}
"""
with tempfile.TemporaryDirectory(prefix="vita3k-system-events-") as folder:
    root = Path(folder)
    (root / "test.cpp").write_text(prelude + "\n".join(bodies) + tests)
    subprocess.run(["clang++", "-std=c++23", "-O1", "-g", "-fsanitize=address,undefined",
                    str(root / "test.cpp"), "-o", str(root / "test")], check=True)
    subprocess.run([str(root / "test")], check=True)
