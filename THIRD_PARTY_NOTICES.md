# Third-party notices

Vendored dependencies and their licenses. Each vendored copy keeps its
upstream license file / header; nothing here modifies them.

## Dear ImGui 1.92.1 (third_party/imgui)

MIT License. Full text: third_party/imgui/LICENSE.txt.
Copyright (c) 2014-2025 Omar Cornut and ImGui contributors.

## stb_image_write.h (third_party/stb_image_write.h)

Public domain / MIT dual license, as stated in the file header.

## stb_image.h v2.30 (third_party/stb_image.h)

Public domain / MIT dual license, as stated in the file header.

## UE4SS / UEPseudo (仅引擎输入版构建，未 vendored)

`PBZFM_ENGINE_PORT=ON` 的构建会链接并加载 [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
（及其反射层 UEPseudo），用于调用引擎的 `bBlockInput` / `SetIgnoreMoveInput` /
`SetIgnoreLookInput` / `bShowMouseCursor`。这些库不打进本仓库，也不随默认发布包分发。

MIT License, Copyright (c) 2022 Narknon（fork 自 RE-UE4SS，同为 MIT）。
默认构建（`PBZFM_ENGINE_PORT=OFF`）不链接、不依赖 UE4SS，不受此节影响。
