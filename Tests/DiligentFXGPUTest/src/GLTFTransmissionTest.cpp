/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "PBR/interface/GLTF_PBR_Renderer.hpp"
#include "GPUTestingEnvironment.hpp"
#include "GraphicsUtilities.h"

#include "gtest/gtest.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Diligent;
using namespace Diligent::Testing;

namespace
{

namespace ShaderTypes
{
#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/PBR/public/PBR_Structures.fxh"
#include "Shaders/PBR/private/RenderPBR_Structures.fxh"
} // namespace ShaderTypes

using Renderer = GLTF_PBR_Renderer;

constexpr Uint32 TargetSize = 32;
const float4     Background{0.2f, 0.4f, 0.8f, 1.f};
const float4     SceneColor{0.8f, 0.4f, 0.2f, 1.f};

class GLTFTransmissionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_pDevice  = GPUTestingEnvironment::GetInstance()->GetDevice();
        m_pContext = GPUTestingEnvironment::GetInstance()->GetDeviceContext();

        Renderer::CreateInfo CI;
        CI.EnableIBL                 = false;
        CI.EnableAO                  = false;
        CI.EnableEmissive            = false;
        CI.EnableTransmission        = true;
        CI.MaxLightCount             = 0;
        CI.MaxJointCount             = 0;
        CI.MaxActiveMorphTargetCount = 0;
        CI.NumRenderTargets          = 1;
        CI.RTVFormats[0]             = TEX_FORMAT_RGBA32_FLOAT;
        CI.DSVFormat                 = TEX_FORMAT_D32_FLOAT;
        m_Renderer.reset(new Renderer{m_pDevice, nullptr, m_pContext, CI});

        CreateUniformBuffer(m_pDevice, sizeof(ShaderTypes::PBRFrameAttribs), "Transmission frame", &m_pFrame, USAGE_DEFAULT);
        ASSERT_NE(nullptr, m_pFrame);
        ShaderTypes::PBRFrameAttribs Frame{};
        Frame.Camera.mViewProj           = float4x4::Identity();
        Frame.Camera.mView               = float4x4::Identity();
        Frame.Camera.f4Position          = float4{0, 0, -2, 1};
        Frame.Camera.f4ViewportSize      = float4{TargetSize, TargetSize, 1.f / TargetSize, 1.f / TargetSize};
        Frame.Renderer.OcclusionStrength = 1.f;
        Frame.Renderer.EmissionScale     = 1.f;
        m_pContext->UpdateBuffer(m_pFrame, 0, sizeof(Frame), &Frame, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        StateTransitionDesc FrameBarrier{m_pFrame, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_CONSTANT_BUFFER, STATE_TRANSITION_FLAG_UPDATE_STATE};
        m_pContext->TransitionResourceStates(1, &FrameBarrier);

        TextureDesc Desc;
        Desc.Name      = "Transmission output";
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = TargetSize;
        Desc.Height    = TargetSize;
        Desc.Format    = TEX_FORMAT_RGBA32_FLOAT;
        Desc.BindFlags = BIND_RENDER_TARGET;
        m_pDevice->CreateTexture(Desc, nullptr, &m_pOutput);
        ASSERT_NE(nullptr, m_pOutput);
        Desc.Name           = "Transmission readback";
        Desc.Usage          = USAGE_STAGING;
        Desc.BindFlags      = BIND_NONE;
        Desc.CPUAccessFlags = CPU_ACCESS_READ;
        m_pDevice->CreateTexture(Desc, nullptr, &m_pReadback);
        ASSERT_NE(nullptr, m_pReadback);
        Desc.Name           = "Transmission depth";
        Desc.Usage          = USAGE_DEFAULT;
        Desc.BindFlags      = BIND_DEPTH_STENCIL;
        Desc.CPUAccessFlags = CPU_ACCESS_NONE;
        Desc.Format         = TEX_FORMAT_D32_FLOAT;
        m_pDevice->CreateTexture(Desc, nullptr, &m_pDepth);
        ASSERT_NE(nullptr, m_pDepth);

        m_pSceneColor = MakeSceneColor(SceneColor);
        ASSERT_NE(nullptr, m_pSceneColor);
        m_Params.Flags = Renderer::PSO_FLAG_USE_VERTEX_NORMALS |
            Renderer::PSO_FLAG_ENABLE_TRANSMISSION |
            Renderer::PSO_FLAG_ENABLE_TRANSMISSION_COMPOSITE;
        m_Params.pTransmissionSceneColorSRV = m_pSceneColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        m_Params.pCameraPosition            = &m_CameraPosition;
    }

    void TearDown() override
    {
        m_pContext->WaitForIdle();
        m_pContext->InvalidateState();
    }

    RefCntAutoPtr<ITexture> MakeSceneColor(const float4& Color, bool Mips = false)
    {
        TextureDesc Desc;
        Desc.Name      = "Copied transmission scene color";
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = TargetSize;
        Desc.Height    = TargetSize;
        Desc.Format    = TEX_FORMAT_RGBA32_FLOAT;
        Desc.BindFlags = BIND_SHADER_RESOURCE;
        Desc.MipLevels = Mips ? 2 : 1;
        std::vector<float4> Pixels(TargetSize * TargetSize, Color);
        std::vector<float4> RoughPixels(TargetSize * TargetSize / 4, float4{0.1f, 0.7f, 0.3f, 1.f});
        TextureSubResData   Subresources[] = {
            {Pixels.data(), TargetSize * sizeof(float4)},
            {RoughPixels.data(), TargetSize / 2 * sizeof(float4)},
        };
        TextureData             Data{Subresources, Desc.MipLevels};
        RefCntAutoPtr<ITexture> Texture;
        m_pDevice->CreateTexture(Desc, &Data, &Texture);
        StateTransitionDesc Barrier{Texture, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE};
        m_pContext->TransitionResourceStates(1, &Barrier);
        return Texture;
    }

    void LoadScene(const char* AlphaMode = "OPAQUE", float Alpha = 1.f, float IOR = 1.f, bool Overlap = false, float Roughness = 0.f)
    {
        // The nearer red surface is submitted before the farther green surface.
        // With transmission depth writes disabled, only sorting can keep red in front.
        const std::string Material = std::string{"{\"doubleSided\":true,\"alphaMode\":\""} + AlphaMode +
            "\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":" + std::to_string(Roughness) +
            ",\"baseColorFactor\":[1," + (Overlap ? "0" : std::string{"1"}) + "," + (Overlap ? "0" : std::string{"1"}) + "," + std::to_string(Alpha) +
            "]},\"extensions\":{\"KHR_materials_transmission\":{\"transmissionFactor\":1},\"KHR_materials_ior\":{\"ior\":" + std::to_string(IOR) + "}}}";
        const std::string Gltf = std::string{R"({"asset":{"version":"2.0"},"buffers":[{"uri":"geometry.bin","byteLength":72}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[3,3,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}],"materials":[)"} + Material +
            R"(,{"doubleSided":true,"pbrMetallicRoughness":{"metallicFactor":0,"roughnessFactor":0,"baseColorFactor":[0,1,0,1]},"extensions":{"KHR_materials_transmission":{"transmissionFactor":1},"KHR_materials_ior":{"ior":1}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"material":0}]},{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"material":1}]}],"nodes":[{"mesh":0,"translation":[0,0,0.25]},{"mesh":1,"translation":[0,0,0.75]}],"scenes":[{"nodes":)" +
            (Overlap ? "[0,1]" : "[0]") + "}],\"scene\":0}";

        GLTF::ModelCreateInfo CI;
        CI.FileName           = "transmission.gltf";
        CI.FileExistsCallback = [](const char*) {
            return true;
        };
        CI.ReadWholeFileCallback = [Gltf](const char* Path, std::vector<unsigned char>& Data, std::string&) {
            if (std::string{Path}.find("geometry.bin") != std::string::npos)
            {
                const float Geometry[] = {-1, -1, 0, -1, 3, 0, 3, -1, 0, 0, 0, -1, 0, 0, -1, 0, 0, -1};
                const auto* Bytes      = reinterpret_cast<const unsigned char*>(Geometry);
                Data.assign(Bytes, Bytes + sizeof(Geometry));
            }
            else
                Data.assign(Gltf.begin(), Gltf.end());
            return true;
        };
        m_Model.reset(new GLTF::Model{m_pDevice, m_pContext, CI});
        ASSERT_TRUE(m_Model->PrepareGPUResources(m_pDevice, m_pContext));
        m_Model->ComputeTransforms(0, m_Transforms);
        m_Bindings = m_Renderer->CreateResourceBindings(*m_Model, m_pFrame);
        ASSERT_FALSE(m_Bindings.MaterialSRB.empty());
        const int ExpectedAlpha = std::strcmp(AlphaMode, "BLEND") == 0 ? GLTF::Material::ALPHA_MODE_BLEND :
                                                                         std::strcmp(AlphaMode, "MASK") == 0 ? GLTF::Material::ALPHA_MODE_MASK : GLTF::Material::ALPHA_MODE_OPAQUE;
        EXPECT_EQ(ExpectedAlpha, m_Model->Materials[0].Attribs.AlphaMode);
    }

    void Draw()
    {
        ITextureView* RTV = m_pOutput->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        ITextureView* DSV = m_pDepth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
        m_pContext->SetRenderTargets(1, &RTV, DSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_pContext->ClearRenderTarget(RTV, &Background.x, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_pContext->ClearDepthStencil(DSV, CLEAR_DEPTH_FLAG, 1.f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_Renderer->Begin(m_pContext);
        m_Renderer->Render(m_pContext, *m_Model, m_Transforms, nullptr, m_Params, &m_Bindings);
        m_pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    }

    void Render(float4& Result)
    {
        ASSERT_NO_FATAL_FAILURE(Draw());
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = m_pOutput;
        Copy.pDstTexture              = m_pReadback;
        Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        m_pContext->CopyTexture(Copy);
        m_pContext->WaitForIdle();
        MappedTextureSubresource Mapped;
        const auto               Flags = m_pDevice->GetDeviceInfo().IsGLDevice() || m_pDevice->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D11 ? MAP_FLAG_NONE : MAP_FLAG_DO_NOT_WAIT;
        m_pContext->MapTextureSubresource(m_pReadback, 0, 0, MAP_READ, Flags, nullptr, Mapped);
        ASSERT_NE(nullptr, Mapped.pData);
        const auto* Pixel = static_cast<const Uint8*>(Mapped.pData) + TargetSize / 2 * Mapped.Stride + TargetSize / 2 * sizeof(float4);
        std::memcpy(&Result, Pixel, sizeof(Result));
        m_pContext->UnmapTextureSubresource(m_pReadback, 0, 0);
    }

    void ExpectColor(const float4& Expected)
    {
        float4 Actual;
        ASSERT_NO_FATAL_FAILURE(Render(Actual));
        for (int Channel = 0; Channel < 4; ++Channel)
            EXPECT_NEAR(Expected[Channel], Actual[Channel], 0.003f) << "Channel " << Channel;
    }

    void ExpectDepth(float Expected)
    {
        TextureDesc Desc    = m_pDepth->GetDesc();
        Desc.Name           = "Transmission depth readback";
        Desc.BindFlags      = BIND_NONE;
        Desc.Usage          = USAGE_STAGING;
        Desc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<ITexture> Readback;
        m_pDevice->CreateTexture(Desc, nullptr, &Readback);
        ASSERT_NE(nullptr, Readback);
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = m_pDepth;
        Copy.pDstTexture              = Readback;
        Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        m_pContext->CopyTexture(Copy);
        m_pContext->WaitForIdle();
        MappedTextureSubresource Mapped;
        const auto               Flags = m_pDevice->GetDeviceInfo().IsGLDevice() || m_pDevice->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D11 ? MAP_FLAG_NONE : MAP_FLAG_DO_NOT_WAIT;
        m_pContext->MapTextureSubresource(Readback, 0, 0, MAP_READ, Flags, nullptr, Mapped);
        ASSERT_NE(nullptr, Mapped.pData);
        const auto* Pixel  = static_cast<const Uint8*>(Mapped.pData) + TargetSize / 2 * Mapped.Stride + TargetSize / 2 * sizeof(float);
        float       Actual = 0;
        std::memcpy(&Actual, Pixel, sizeof(Actual));
        m_pContext->UnmapTextureSubresource(Readback, 0, 0);
        EXPECT_NEAR(Expected, Actual, 0.0001f);
    }

    GPUTestingEnvironment::ScopedReset m_Reset;
    IRenderDevice*                     m_pDevice  = nullptr;
    IDeviceContext*                    m_pContext = nullptr;
    std::unique_ptr<Renderer>          m_Renderer;
    std::unique_ptr<GLTF::Model>       m_Model;
    GLTF::ModelTransforms              m_Transforms;
    Renderer::ModelResourceBindings    m_Bindings;
    Renderer::RenderInfo               m_Params;
    float3                             m_CameraPosition{0, 0, -2};
    RefCntAutoPtr<IBuffer>             m_pFrame;
    RefCntAutoPtr<ITexture>            m_pOutput, m_pReadback, m_pDepth, m_pSceneColor;
};

TEST_F(GLTFTransmissionTest, OpaqueCoverageUsesSceneColor)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 0.1f));
    ExpectColor(SceneColor);
}

TEST_F(GLTFTransmissionTest, AuthoredBlendCoverage)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("BLEND", 0.25f));
    ExpectColor(SceneColor * 0.25f + Background * 0.75f);
}

TEST_F(GLTFTransmissionTest, MaskDiscardsBelowCutoff)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("MASK", 0.25f));
    ExpectColor(Background);
}

TEST_F(GLTFTransmissionTest, MaskKeepsFullCoverageAboveCutoff)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("MASK", 0.75f));
    ExpectColor(SceneColor);
}

TEST_F(GLTFTransmissionTest, FresnelAttenuatesTransmission)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 1.f, 1.5f));
    ExpectColor(float4{SceneColor.x * 0.96f, SceneColor.y * 0.96f, SceneColor.z * 0.96f, 1.f});
}

TEST_F(GLTFTransmissionTest, RoughnessSelectsSceneMip)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 1.f, 1.f, false, 1.f));
    auto MippedScene                    = MakeSceneColor(SceneColor, true);
    m_Params.pTransmissionSceneColorSRV = MippedScene->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    ExpectColor(float4{0.1f, 0.7f, 0.3f, 1.f});
}

TEST_F(GLTFTransmissionTest, PartialRoughnessUsesShortMipChain)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 1.f, 1.f, false, 0.5f));
    auto MippedScene                    = MakeSceneColor(SceneColor, true);
    m_Params.pTransmissionSceneColorSRV = MippedScene->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    ExpectColor(SceneColor * 0.5f + float4{0.1f, 0.7f, 0.3f, 1.f} * 0.5f);
}

TEST_F(GLTFTransmissionTest, RestrictedSceneViewUsesVisibleMipRange)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 1.f, 1.f, false, 1.f));
    auto            MippedScene = MakeSceneColor(SceneColor, true);
    TextureViewDesc ViewDesc;
    ViewDesc.ViewType        = TEXTURE_VIEW_SHADER_RESOURCE;
    ViewDesc.MostDetailedMip = 1;
    ViewDesc.NumMipLevels    = 1;
    RefCntAutoPtr<ITextureView> View;
    MippedScene->CreateView(ViewDesc, &View);
    ASSERT_NE(nullptr, View);
    m_Params.pTransmissionSceneColorSRV = View;
    ExpectColor(float4{0.1f, 0.7f, 0.3f, 1.f});
}

TEST_F(GLTFTransmissionTest, SortsOverlappingOpaqueTransmission)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("OPAQUE", 1.f, 1.f, true));
    ExpectColor(float4{SceneColor.x, 0, 0, 1});
}

TEST_F(GLTFTransmissionTest, SceneColorCanChangeBetweenDraws)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    auto NextScene = MakeSceneColor(float4{0.3f, 0.6f, 0.9f, 1.f});
    ASSERT_NO_FATAL_FAILURE(Draw());
    m_Params.pTransmissionSceneColorSRV = NextScene->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    ExpectColor(float4{0.3f, 0.6f, 0.9f, 1.f});
}

TEST_F(GLTFTransmissionTest, MissingSceneColorDoesNotUseStaleBinding)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    ExpectColor(SceneColor);
    m_Params.pTransmissionSceneColorSRV = nullptr;
    ExpectColor(float4{0, 0, 0, 1});
}

TEST_F(GLTFTransmissionTest, TransmissionFlagDisablesComposite)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    m_Params.Flags &= ~Renderer::PSO_FLAG_ENABLE_TRANSMISSION;
    ExpectColor(float4{0, 0, 0, 1});
}

TEST_F(GLTFTransmissionTest, CompositeFlagDisablesSceneRead)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    m_Params.Flags &= ~Renderer::PSO_FLAG_ENABLE_TRANSMISSION_COMPOSITE;
    ExpectColor(float4{0, 0, 0, 1});
}

TEST_F(GLTFTransmissionTest, SurfaceAndAlphaFiltersExcludeTransmission)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    m_Params.SurfaceFlags = Renderer::RenderInfo::SURFACE_FLAG_OPAQUE;
    ExpectColor(Background);
    m_Params.SurfaceFlags = Renderer::RenderInfo::SURFACE_FLAG_TRANSMISSION;
    m_Params.AlphaModes   = Renderer::RenderInfo::ALPHA_MODE_FLAG_BLEND;
    ExpectColor(Background);
}

TEST_F(GLTFTransmissionTest, CompositePreservesOpaqueDepth)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene());
    ExpectColor(SceneColor);
    ExpectDepth(1.f);
    m_Params.pTransmissionSceneColorSRV = nullptr;
    ExpectColor(float4{0, 0, 0, 1});
    const auto& NDC = m_pDevice->GetDeviceInfo().NDC;
    ExpectDepth(0.25f * NDC.ZtoDepthScale + NDC.GetZtoDepthBias());
}

TEST_F(GLTFTransmissionTest, CompositeBypassesOITLayers)
{
    ASSERT_NO_FATAL_FAILURE(LoadScene("BLEND", 0.25f));
    Renderer::CreateInfo CI;
    static_cast<PBR_Renderer::CreateInfo&>(CI) = m_Renderer->GetSettings();
    CI.OITLayerCount                           = 4;
    CI.NumRenderTargets                        = 1;
    CI.RTVFormats[0]                           = TEX_FORMAT_RGBA32_FLOAT;
    CI.DSVFormat                               = TEX_FORMAT_D32_FLOAT;
    m_Renderer.reset(new Renderer{m_pDevice, nullptr, m_pContext, CI});
    m_Bindings = m_Renderer->CreateResourceBindings(*m_Model, m_pFrame);
    ExpectColor(SceneColor * 0.25f + Background * 0.75f);
}

TEST(GLTFTransmissionFlagsTest, RendererSettingsDisableComposite)
{
    Renderer::CreateInfo CI;
    CI.EnableTransmission = false;
    EXPECT_EQ(Renderer::PSO_FLAG_NONE, Renderer::GetEnabledPSOFlags(CI) & (Renderer::PSO_FLAG_ALL_TRANSMISSION | Renderer::PSO_FLAG_ENABLE_TRANSMISSION_COMPOSITE));
    CI.EnableTransmission = true;
    EXPECT_EQ(Renderer::PSO_FLAG_ENABLE_TRANSMISSION_COMPOSITE,
              Renderer::GetEnabledPSOFlags(CI) & Renderer::PSO_FLAG_ENABLE_TRANSMISSION_COMPOSITE);
}

} // namespace
