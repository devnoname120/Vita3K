from pathlib import Path
import re
import sys


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing {signature}")
    brace = source.find("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated {signature}")


def main() -> int:
    source = Path(sys.argv[1]).read_text()
    init = function_body(source, "ExitCode init(const Root &root_paths, bool use_stdout)")
    handler = function_body(source, "static void terminate_handler()")

    required_source = (
        "static std::once_flag s_terminate_handler_once;",
        "static std::terminate_handler s_previous_terminate = nullptr;",
    )
    for required in required_source:
        if required not in source:
            raise AssertionError(f"missing {required}")

    if source.count("std::set_terminate(") != 1:
        raise AssertionError("terminate handler must be installed exactly once in source")
    if "std::call_once(s_terminate_handler_once" not in init:
        raise AssertionError("logging reinitialization can replace the saved terminate handler")
    if "s_previous_terminate = std::set_terminate(terminate_handler);" not in init:
        raise AssertionError("previous terminate handler is not captured by the one-time install")
    if "old_terminate = std::set_terminate" in source:
        raise AssertionError("per-init terminate handler replacement remains")
    if "s_previous_terminate != terminate_handler" not in handler:
        raise AssertionError("terminate handler lacks a self-recursion guard")
    if "std::abort();" not in handler:
        raise AssertionError("terminate handler may return after a previous handler returns")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
