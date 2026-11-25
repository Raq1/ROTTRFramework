#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>
#include <fstream>
int main(int argc, char** argv) {
  std::ifstream in(argv[1], std::ios::binary); std::vector<char> d((std::istreambuf_iterator<char>(in)), {});
  size_t at = strtoul(argv[2], 0, 10); unsigned size = *(unsigned*)&d[at + 24];
  ID3DBlob* out = nullptr;
  HRESULT hr = D3DDisassemble(&d[at], size, 0, nullptr, &out);
  if (FAILED(hr)) { printf("fail %08lx\n", hr); return 1; }
  fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), stdout);
}
