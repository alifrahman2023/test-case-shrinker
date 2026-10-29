class Shrink < Formula
  desc "Reduce wrong-answer integer-array inputs"
  homepage "https://github.com/alifrahman2023/test-case-shrinker"
  url "https://github.com/alifrahman2023/test-case-shrinker.git", tag: "v0.1.0"
  version "0.1.0"

  depends_on "cmake" => :build
  depends_on arch: :arm64
  depends_on macos: :sequoia

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
                    "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF"
    system "cmake", "--build", "build", "--parallel", "3"
    system "cmake", "--install", "build"
  end

  test do
    assert_equal "shrink #{version}\n", shell_output("#{bin}/shrink --version")
    assert_match "Usage: shrink", shell_output("#{bin}/shrink --help")
    (testpath/"solution.cpp").write <<~CPP
      #include <iostream>
      int main() {
        int n, value, previous = 0, answer = 0;
        std::cin >> n;
        for (int i = 0; i < n; ++i) {
          std::cin >> value;
          if (i == 0 || value != previous) ++answer;
          previous = value;
        }
        std::cout << answer << '\\n';
      }
    CPP
    (testpath/"reference.cpp").write <<~CPP
      #include <iostream>
      #include <set>
      int main() {
        int n, value;
        std::set<int> values;
        std::cin >> n;
        for (int i = 0; i < n; ++i) { std::cin >> value; values.insert(value); }
        std::cout << values.size() << '\\n';
      }
    CPP
    system ENV.cxx, "solution.cpp", "-o", "solution"
    system ENV.cxx, "reference.cpp", "-o", "reference"
    (testpath/"original.txt").write "4\n42 -7 42 9\n"
    system bin/"shrink", "--input", "original.txt", "--solution", "./solution",
           "--reference", "./reference", "--min-length", "0", "--max-length", "10",
           "--min-value=-100", "--max-value", "100", "--output", "reduced.txt", "--quiet"
    assert_equal "3\n0 1 0\n", (testpath/"reduced.txt").read
  end
end
