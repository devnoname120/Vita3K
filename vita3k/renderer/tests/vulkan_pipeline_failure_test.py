from pathlib import Path
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
    body = function_body(source, "vk::Pipeline PipelineCache::compile_pipeline(")

    required = (
        "vk::Pipeline pipeline;",
        "state.device.createGraphicsPipelines(",
        "pipeline_cache, 1, &pipeline_info, nullptr, &pipeline",
        "result != vk::Result::eSuccess",
        "vk::to_string(result)",
        "return pipeline;",
    )
    for item in required:
        if item not in body:
            raise AssertionError(f"pipeline creation failure path is missing {item}")
    if "state.device.createGraphicsPipeline(" in body:
        raise AssertionError("throwing enhanced Vulkan-Hpp pipeline overload remains")
    if "return result.value;" in body:
        raise AssertionError("pipeline path still relies on enhanced ResultValue")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
