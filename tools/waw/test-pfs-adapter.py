from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
text = (ROOT / "vita3k/packages/src/pkg.cpp").read_text()
start = text.index("using InstallPfsProgressCallback")
end = text.index("bool decrypt_install_nonpdrm", start)
adapter = text[start:end]
prelude = r"""
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace fs { struct path { std::string value; std::string string() const { return value; } }; }
enum class F00DEncryptorTypes { native };
static int result_code, calls;
static void check(std::string &z, std::string &s, std::string &d, F00DEncryptorTypes t, std::string &a) {
    assert(z == "license" && s == "source" && d == "destination" && a == "argument" && t == F00DEncryptorTypes::native);
    ++calls;
}
#if PROVIDER_HAS_PROGRESS
using PfsProgressCallback = std::function<void(std::uint64_t, std::uint64_t, const std::string &)>;
static int execute(std::string &z, std::string &s, std::string &d, F00DEncryptorTypes t, std::string &a, PfsProgressCallback p = {}) {
    check(z,s,d,t,a);
    if (result_code >= 0 && p) { p(1,4,"file"); p(4,4,"file"); }
    return result_code;
}
#else
static int execute(std::string &z, std::string &s, std::string &d, F00DEncryptorTypes t, std::string &a) {
    check(z,s,d,t,a); return result_code;
}
#endif
"""
test = r"""
int main() {
    std::string z = "license", a = "argument";
    fs::path s{"source"}, d{"destination"};
    for (int result : {0, -7}) {
        result_code = result;
        calls = 0;
        assert(execute(z,s,d,F00DEncryptorTypes::native,a) == result);
        assert(calls == 1);
        std::vector<float> stages;
        InstallPfsProgressCallback progress = [&](std::uint64_t n, std::uint64_t total, const std::string &) { stages.push_back(float(n)/total); };
        assert(execute(z,s,d,F00DEncryptorTypes::native,a,progress) == result);
        assert(calls == 2);
        if (result < 0) assert(stages.empty());
        else {
            assert(stages.back() == 1.f);
            assert(stages.size() == (PROVIDER_HAS_PROGRESS ? 2u : 1u));
        }
    }
}
"""
with tempfile.TemporaryDirectory(prefix="waw-pfs-adapter-") as folder:
    temp = Path(folder)
    (temp / "test.cpp").write_text(prelude + adapter + test)
    for provider in (0,1):
        exe = temp / ("test" + str(provider))
        subprocess.run(["clang++", "-std=c++23", "-O1", "-g", "-fsanitize=address,undefined",
                        "-DPROVIDER_HAS_PROGRESS=" + str(provider), str(temp / "test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("Both provider APIs pass: arguments, success/error propagation, optional callback and progress reporting (8 combinations).")
