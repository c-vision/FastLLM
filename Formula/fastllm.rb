class Fastllm < Formula
  desc "Run MLX and GGUF language models locally on Apple Silicon, with an OpenAI-compatible server"
  homepage "https://github.com/c-vision/FastLLM"
  url "https://github.com/c-vision/FastLLM/releases/download/v0.1.0/fastllm-0.1.0-macos-arm64.tar.gz"
  sha256 "5bff25702cb446e876164dc264c231fa0a21ad9b77b8499c35ee6df5efedff9f"
  version "0.1.0"
  license "Apache-2.0"

  depends_on arch: :arm64
  depends_on :macos
  # The inference engine is a separate formula, so several programs can share one copy.
  depends_on "llmenv"

  def install
    bin.install "bin/fastllm"
    pkgshare.install "share/fastllm/ds4", "share/fastllm/REQUIRES"
    doc.install Dir["share/doc/fastllm/*"]
    # no link to the engine is needed: the binary has /opt/homebrew/opt/llmenv/lib/llmenv in its rpath
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/fastllm version")
    assert_match "installation OK", shell_output("#{bin}/fastllm doctor --install")
  end
end
