// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "imgui_impl_impeller.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <vector>

#include "impeller/geometry/scalar.h"
#include "impeller/geometry/vector.h"
#include "impeller/playground/imgui/imgui_raster.frag.h"
#include "impeller/playground/imgui/imgui_raster.vert.h"
#include "third_party/imgui/imgui.h"

#include "impeller/core/allocator.h"
#include "impeller/core/formats.h"
#include "impeller/core/range.h"
#include "impeller/core/sampler.h"
#include "impeller/core/texture.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/core/vertex_buffer.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"
#include "impeller/geometry/size.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/pipeline_builder.h"
#include "impeller/renderer/pipeline_descriptor.h"
#include "impeller/renderer/pipeline_library.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"

struct ImGui_ImplImpeller_Data {
  std::shared_ptr<impeller::Context> context;
  std::shared_ptr<impeller::Texture> font_texture;
  std::shared_ptr<impeller::Pipeline<impeller::PipelineDescriptor>> pipeline;
  std::shared_ptr<const impeller::Sampler> sampler;
};

static ImGui_ImplImpeller_Data* ImGui_ImplImpeller_GetBackendData() {
  return ImGui::GetCurrentContext()
             ? static_cast<ImGui_ImplImpeller_Data*>(
                   ImGui::GetIO().BackendRendererUserData)
             : nullptr;
}

bool ImGui_ImplImpeller_Init(
    const std::shared_ptr<impeller::Context>& context) {
  ImGuiIO& io = ImGui::GetIO();
  IM_ASSERT(io.BackendRendererUserData == nullptr &&
            "Already initialized a renderer backend!");

  // Setup backend capabilities flags
  auto* bd = new ImGui_ImplImpeller_Data();
  io.BackendRendererUserData = reinterpret_cast<void*>(bd);
  io.BackendRendererName = "imgui_impl_impeller";
  io.BackendFlags |=
      ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the
                                               // ImDrawCmd::VtxOffset field,
                                               // allowing for large meshes.

  bd->context = context;

  // Generate/upload the font atlas.
  {
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    auto texture_descriptor = impeller::TextureDescriptor{};
    texture_descriptor.storage_mode = impeller::StorageMode::kHostVisible;
    texture_descriptor.format = impeller::PixelFormat::kR8G8B8A8UNormInt;
    texture_descriptor.size = {width, height};
    texture_descriptor.mip_count = 1u;

    bd->font_texture =
        context->GetResourceAllocator()->CreateTexture(texture_descriptor);
    IM_ASSERT(bd->font_texture != nullptr &&
              "Could not allocate ImGui font texture.");
    bd->font_texture->SetLabel("ImGui Font Texture");

    [[maybe_unused]] bool uploaded = bd->font_texture->SetContents(
        pixels, texture_descriptor.GetByteSizeOfBaseMipLevel());
    IM_ASSERT(uploaded &&
              "Could not upload ImGui font texture to device memory.");
  }

  // Build the raster pipeline.
  {
    auto desc = impeller::PipelineBuilder<impeller::ImguiRasterVertexShader,
                                          impeller::ImguiRasterFragmentShader>::
        MakeDefaultPipelineDescriptor(*context);
    IM_ASSERT(desc.has_value() && "Could not create Impeller pipeline");
    if (desc.has_value()) {  // Needed to silence clang-tidy check
                             // bugprone-unchecked-optional-access.
      desc->ClearStencilAttachments();
      desc->ClearDepthAttachment();
    }

    bd->pipeline =
        context->GetPipelineLibrary()->GetPipeline(std::move(desc)).Get();
    IM_ASSERT(bd->pipeline != nullptr && "Could not create ImGui pipeline.");

    bd->sampler = context->GetSamplerLibrary()->GetSampler({});
    IM_ASSERT(bd->pipeline != nullptr && "Could not create ImGui sampler.");
  }

  return true;
}

void ImGui_ImplImpeller_Shutdown() {
  auto* bd = ImGui_ImplImpeller_GetBackendData();
  IM_ASSERT(bd != nullptr &&
            "No renderer backend to shutdown, or already shutdown?");
  delete bd;
}

void ImGui_ImplImpeller_RenderDrawData(ImDrawData* draw_data,
                                       impeller::RenderPass& render_pass) {
  if (draw_data->CmdListsCount == 0) {
    return;  // Nothing to render.
  }

  using VS = impeller::ImguiRasterVertexShader;
  using FS = impeller::ImguiRasterFragmentShader;

  auto* bd = ImGui_ImplImpeller_GetBackendData();
  IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplImpeller_Init()?");

  size_t total_vtx_bytes = draw_data->TotalVtxCount * sizeof(VS::PerVertexData);
  size_t total_idx_bytes = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
  if (!total_vtx_bytes || !total_idx_bytes) {
    return;  // Nothing to render.
  }

  // Allocate buffer for vertices + indices.
  impeller::DeviceBufferDescriptor buffer_desc;
  buffer_desc.size = total_vtx_bytes + total_idx_bytes;
  buffer_desc.storage_mode = impeller::StorageMode::kHostVisible;

  auto buffer = bd->context->GetResourceAllocator()->CreateBuffer(buffer_desc);
  buffer->SetLabel(impeller::SPrintF("ImGui vertex+index buffer"));

  auto display_rect = impeller::Rect::MakeXYWH(
      draw_data->DisplayPos.x, draw_data->DisplayPos.y,
      draw_data->DisplaySize.x, draw_data->DisplaySize.y);

  auto viewport = impeller::Viewport{
      .rect = impeller::Rect::MakeXYWH(
          display_rect.origin.x * draw_data->FramebufferScale.x,
          display_rect.origin.y * draw_data->FramebufferScale.y,
          display_rect.size.width * draw_data->FramebufferScale.x,
          display_rect.size.height * draw_data->FramebufferScale.y)};

  // Allocate vertex shader uniform buffer.
  VS::UniformBuffer uniforms;
  uniforms.mvp = impeller::Matrix::MakeOrthographic(display_rect.size)
                     .Translate(-display_rect.origin);
  auto vtx_uniforms =
      render_pass.GetTransientsBuffer().EmplaceUniform(uniforms);

  size_t vertex_buffer_offset = 0;
  size_t index_buffer_offset = total_vtx_bytes;

  for (int draw_list_i = 0; draw_list_i < draw_data->CmdListsCount;
       draw_list_i++) {
    const ImDrawList* cmd_list = draw_data->CmdLists[draw_list_i];

    // Convert ImGui's per-vertex data (`ImDrawVert`) into the per-vertex data
    // required by the shader (`VS::PerVectexData`). The only difference is that
    // `ImDrawVert` uses an `int` for the color and the impeller shader uses 4
    // floats.

    // TODO(102778): Remove the need for this by adding support for attribute
    //               mapping of uint32s host-side to vec4s shader-side in
    //               impellerc.
    std::vector<VS::PerVertexData> vtx_data;
    vtx_data.reserve(cmd_list->VtxBuffer.size());
    for (const auto& v : cmd_list->VtxBuffer) {
      ImVec4 color = ImGui::ColorConvertU32ToFloat4(v.col);
      vtx_data.push_back({{v.pos.x, v.pos.y},  //
                          {v.uv.x, v.uv.y},    //
                          {color.x, color.y, color.z, color.w}});
    }

    auto draw_list_vtx_bytes =
        static_cast<size_t>(vtx_data.size() * sizeof(VS::PerVertexData));
    auto draw_list_idx_bytes =
        static_cast<size_t>(cmd_list->IdxBuffer.size_in_bytes());
    if (!buffer->CopyHostBuffer(reinterpret_cast<uint8_t*>(vtx_data.data()),
                                impeller::Range{0, draw_list_vtx_bytes},
                                vertex_buffer_offset)) {
      IM_ASSERT(false && "Could not copy vertices to buffer.");
    }
    if (!buffer->CopyHostBuffer(
            reinterpret_cast<uint8_t*>(cmd_list->IdxBuffer.Data),
            impeller::Range{0, draw_list_idx_bytes}, index_buffer_offset)) {
      IM_ASSERT(false && "Could not copy indices to buffer.");
    }

    for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
      const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

      if (pcmd->UserCallback) {
        pcmd->UserCallback(cmd_list, pcmd);
      } else {
        // Make the clip rect relative to the viewport.
        auto clip_rect = impeller::Rect::MakeLTRB(
            (pcmd->ClipRect.x - draw_data->DisplayPos.x) *
                draw_data->FramebufferScale.x,
            (pcmd->ClipRect.y - draw_data->DisplayPos.y) *
                draw_data->FramebufferScale.y,
            (pcmd->ClipRect.z - draw_data->DisplayPos.x) *
                draw_data->FramebufferScale.x,
            (pcmd->ClipRect.w - draw_data->DisplayPos.y) *
                draw_data->FramebufferScale.y);
        {
          // Clamp the clip to the viewport bounds.
          auto visible_clip = clip_rect.Intersection(viewport.rect);
          if (!visible_clip.has_value()) {
            continue;  // Nothing to render.
          }
          clip_rect = visible_clip.value();
        }
        {
          // Clamp the clip to ensure it never goes outside of the render
          // target.
          auto visible_clip = clip_rect.Intersection(
              impeller::Rect::MakeSize(render_pass.GetRenderTargetSize()));
          if (!visible_clip.has_value()) {
            continue;  // Nothing to render.
          }
          clip_rect = visible_clip.value();
        }

        impeller::Command cmd;
        DEBUG_COMMAND_INFO(cmd,
                           impeller::SPrintF("ImGui draw list %d (command %d)",
                                             draw_list_i, cmd_i));

        cmd.viewport = viewport;
        cmd.scissor = impeller::IRect(clip_rect);

        cmd.pipeline = bd->pipeline;
        VS::BindUniformBuffer(cmd, vtx_uniforms);
        FS::BindTex(cmd, bd->font_texture, bd->sampler);

        size_t vb_start =
            vertex_buffer_offset + pcmd->VtxOffset * sizeof(ImDrawVert);

        impeller::VertexBuffer vertex_buffer;
        vertex_buffer.vertex_buffer = {
            .buffer = buffer,
            .range = impeller::Range(vb_start, draw_list_vtx_bytes - vb_start)};
        vertex_buffer.index_buffer = {
            .buffer = buffer,
            .range = impeller::Range(
                index_buffer_offset + pcmd->IdxOffset * sizeof(ImDrawIdx),
                pcmd->ElemCount * sizeof(ImDrawIdx))};
        vertex_buffer.vertex_count = pcmd->ElemCount;
        vertex_buffer.index_type = impeller::IndexType::k16bit;
        cmd.BindVertices(vertex_buffer);
        cmd.base_vertex = pcmd->VtxOffset;

        render_pass.AddCommand(std::move(cmd));
      }
    }

    vertex_buffer_offset += draw_list_vtx_bytes;
    index_buffer_offset += draw_list_idx_bytes;
  }
}
