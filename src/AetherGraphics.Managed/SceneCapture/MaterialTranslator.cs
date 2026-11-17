using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace AetherGraphics {
	/// <summary>
	/// Turns a Unity material into Aether's material description (AeMaterialDesc). Independent of HDRP: nothing of
	/// HDRP's material evaluation is called; only the material's own properties are read.
	///
	/// The class of a material is decided by the properties its shader really has (the signatures below come from the
	/// survey of the game's shaders, docs/architecture/material-translation.md), not by the shader's name alone.
	/// A shader none of the signatures fits becomes "unsupported": it is drawn with the diagnostic material and counted,
	/// never dropped. Where the meaning of a property is an assumption (no shader source), the document says so.
	/// </summary>
	internal static class MaterialTranslator {
		private static readonly int BaseColor = Shader.PropertyToID("_BaseColor");
		private static readonly int BaseColorMap = Shader.PropertyToID("_BaseColorMap");
		private static readonly int BaseMap = Shader.PropertyToID("_BaseMap");
		private static readonly int MainTex = Shader.PropertyToID("_MainTex");
		private static readonly int Metallic = Shader.PropertyToID("_Metallic");
		private static readonly int Smoothness = Shader.PropertyToID("_Smoothness");
		private static readonly int MaskMap = Shader.PropertyToID("_MaskMap");
		private static readonly int MetallicRemapMax = Shader.PropertyToID("_MetallicRemapMax");
		private static readonly int SmoothnessRemapMax = Shader.PropertyToID("_SmoothnessRemapMax");
		private static readonly int NormalMap = Shader.PropertyToID("_NormalMap");
		private static readonly int NormalScale = Shader.PropertyToID("_NormalScale");
		private static readonly int EmissiveColor = Shader.PropertyToID("_EmissiveColor");
		private static readonly int EmissiveColorMap = Shader.PropertyToID("_EmissiveColorMap");
		private static readonly int AlphaCutoffEnable = Shader.PropertyToID("_AlphaCutoffEnable");
		private static readonly int AlphaCutoff = Shader.PropertyToID("_AlphaCutoff");
		private static readonly int AlphaClippingThreshold = Shader.PropertyToID("_AlphaClippingThreshold");
		private static readonly int SurfaceType = Shader.PropertyToID("_SurfaceType");
		private static readonly int DoubleSidedEnable = Shader.PropertyToID("_DoubleSidedEnable");
		private static readonly int Roughness = Shader.PropertyToID("_Roughness");
		private static readonly int Metalness = Shader.PropertyToID("_Metalness");
		private static readonly int Clearcoat = Shader.PropertyToID("_Clearcoat");
		private static readonly int ClearCoatEnabled = Shader.PropertyToID("_ClearCoatEnabled");
		private static readonly int SecondaryColor = Shader.PropertyToID("_SecondaryColor");
		private static readonly int Spectrum = Shader.PropertyToID("_Spectrum");
		private static readonly int PackMap = Shader.PropertyToID("_PackMap");
		private static readonly int ColormaskMap = Shader.PropertyToID("_ColormaskMap");
		private static readonly int EmissionMap = Shader.PropertyToID("_EmissionMap");
		private static readonly int EmissionIntensity = Shader.PropertyToID("_EmissionIntensity");
		private static readonly int EmissionMasks = Shader.PropertyToID("_EmissionMasks");
		private static readonly int TireBaseMap = Shader.PropertyToID("_TireBaseMap");
		private static readonly int TirePackMap = Shader.PropertyToID("_TirePackMap");
		private static readonly int TireNormalMap = Shader.PropertyToID("_TireNormalMap");
		private static readonly int StitchesPack = Shader.PropertyToID("Texture2D_67B10B5E");
		private static readonly int DiffusionProfileHash = Shader.PropertyToID("_DiffusionProfileHash");
		private static readonly int MotionVectorAngular = Shader.PropertyToID("_MotionVectorAngular");
		private static readonly int MaxBaseColor = Shader.PropertyToID("_BASE_COLOR");
		private static readonly int MaxBaseColorMap = Shader.PropertyToID("_BASE_COLOR_MAP");
		private static readonly int MaxRoughness = Shader.PropertyToID("_REFLECTIONS_ROUGHNESS");
		private static readonly int MaxMetalness = Shader.PropertyToID("_METALNESS");
		private static readonly int ShouldFlipX = Shader.PropertyToID("_ShouldFlipX");
		// HDRP/LayeredLit: up to four layers, each with its own set (suffix 0 .. 3), blended by _LayerMaskMap
		private static readonly int LayerCount = Shader.PropertyToID("_LayerCount");
		private static readonly int BaseColor0 = Shader.PropertyToID("_BaseColor0");
		private static readonly int BaseColorMap0 = Shader.PropertyToID("_BaseColorMap0");
		private static readonly int NormalMap0 = Shader.PropertyToID("_NormalMap0");
		private static readonly int NormalScale0 = Shader.PropertyToID("_NormalScale0");
		private static readonly int MaskMap0 = Shader.PropertyToID("_MaskMap0");
		private static readonly int Metallic0 = Shader.PropertyToID("_Metallic0");
		private static readonly int Smoothness0 = Shader.PropertyToID("_Smoothness0");
		private static readonly int MetallicRemapMax0 = Shader.PropertyToID("_MetallicRemapMax0");
		private static readonly int SmoothnessRemapMax0 = Shader.PropertyToID("_SmoothnessRemapMax0");
		private static readonly int LayerMaskMap = Shader.PropertyToID("_LayerMaskMap");
		private static readonly int VertexColorMode = Shader.PropertyToID("_VertexColorMode");

		// the per layer properties of layers 1 .. 3
		private static int[] LayerIds(string name) => new[] { Shader.PropertyToID(name + "1"), Shader.PropertyToID(name + "2"), Shader.PropertyToID(name + "3") };
		private static readonly int[] LayerBaseColor = LayerIds("_BaseColor"), LayerBaseColorMap = LayerIds("_BaseColorMap"), LayerNormalMap = LayerIds("_NormalMap"),
			LayerNormalScale = LayerIds("_NormalScale"), LayerMaskMapIds = LayerIds("_MaskMap"), LayerMetallic = LayerIds("_Metallic"), LayerSmoothness = LayerIds("_Smoothness"),
			LayerMetallicRemapMax = LayerIds("_MetallicRemapMax"), LayerSmoothnessRemapMax = LayerIds("_SmoothnessRemapMax");

		/// <summary>What a translation produced: the description and the textures it names (slot order).</summary>
		internal struct Result {
			public AeMaterialDesc desc;
			public Texture[] textures; // AeConst.Slot* order, null entries = none
			public Texture[] layerTextures; // HDRP/LayeredLit: the layer mask, then per layer 1 .. 3 base, normal, mask (AeMaterialDesc.layerTextures order)
			public string className;
		}

		private static readonly Texture[] scratch_ = new Texture[5];

		// A renderer can replace any of its material's properties (MaterialPropertyBlock): the game does that for
		// everything the player customises (paint colours, roughness, metalness, clear coat, the livery texture, the
		// lamps' intensities). Every read goes through these helpers: the renderer's value where it has one.
		private static MaterialPropertyBlock over_;

		private static Texture Tex(Material m, int id) {
			if (over_ != null && over_.HasTexture(id)) {
				return over_.GetTexture(id);
			}
			return m.HasProperty(id) ? m.GetTexture(id) : null;
		}

		private static float Float(Material m, int id, float fallback) {
			if (over_ != null && over_.HasFloat(id)) {
				return over_.GetFloat(id);
			}
			return m.HasProperty(id) ? m.GetFloat(id) : fallback;
		}

		/// <summary>A colour property in linear space (a block stores colours the way the shader gets them: linear).</summary>
		private static Color Col(Material m, int id) {
			if (over_ != null && (over_.HasColor(id) || over_.HasVector(id))) {
				return (Color)over_.GetVector(id);
			}
			return m.HasProperty(id) ? m.GetColor(id).linear : Color.white;
		}

		private static Vector4 Vec(Material m, int id) {
			if (over_ != null && over_.HasVector(id)) {
				return over_.GetVector(id);
			}
			return m.HasProperty(id) ? m.GetVector(id) : Vector4.zero;
		}

		private static unsafe void SetColor(float* destination, Color c) {
			destination[0] = c.r;
			destination[1] = c.g;
			destination[2] = c.b;
			destination[3] = c.a;
		}

		/// <summary>Translates a material. `block`: the renderer's own property values (MaterialPropertyBlock), or null.</summary>
		public static unsafe Result Translate(Material m, uint handle, MaterialPropertyBlock block) {
			over_ = block != null && !block.isEmpty ? block : null;
			try {
				return TranslateCore(m, handle);
			} finally {
				over_ = null;
			}
		}

		private static unsafe Result TranslateCore(Material m, uint handle) {
			var r = new Result { textures = new Texture[5], layerTextures = new Texture[10] };
			AeMaterialDesc d = default;
			d.structSize = (uint)sizeof(AeMaterialDesc);
			d.material = handle;
			d.renderQueue = (uint)m.renderQueue;
			d.shaderId = (uint)m.shader.GetInstanceID();
			d.metallic = 0;
			d.roughness = 0.6f;
			d.normalScale = 1;
			d.alphaCutoff = 0.5f;
			d.occlusion = 1;
			d.uvScale[0] = d.uvScale[1] = 1;
			SetColor(d.baseColor, Color.white);
			SetColor(d.secondaryColor, new Color(0, 0, 0, 0));
			for (int i = 0; i < 5; ++i) {
				scratch_[i] = null;
			}
			uint cls = AeConst.ClassUnsupported;
			uint flags = 0;
			int uvProperty = 0;

			// ---- flags HDRP style materials (Lit and the Shader Graphs alike) all carry
			bool transparent = Float(m, SurfaceType, 0) > 0.5f || m.renderQueue >= (int)RenderQueue.Transparent;
			if (Float(m, AlphaCutoffEnable, 0) > 0.5f) {
				flags |= AeConst.MaterialAlphaTest;
			}
			if (Float(m, DoubleSidedEnable, 0) > 0.5f) {
				flags |= AeConst.MaterialDoubleSided;
			}

			bool paintFamily = m.HasProperty(Roughness) && m.HasProperty(Metalness) && m.HasProperty(Clearcoat);
			if (m.HasProperty(MotionVectorAngular)) {
				// the wheels' motion vector proxy: geometry that only exists to write motion vectors
				cls = AeConst.ClassSkip;
			} else if (paintFamily) {
				// the game's paint graphs: _Roughness, _Metalness, _Clearcoat, _BaseColor, _SecondaryColor
				cls = m.HasProperty(Spectrum) ? AeConst.ClassVehiclePaint : m.HasProperty(StitchesPack) ? AeConst.ClassVehicleStitches : AeConst.ClassVehicleRim;
				SetColor(d.baseColor, Col(m, BaseColor));
				SetColor(d.secondaryColor, Col(m, SecondaryColor));
				d.metallic = Float(m, Metalness, 0);
				d.roughness = Float(m, Roughness, 0);
				float coat = Float(m, Clearcoat, 0);
				if (Float(m, ClearCoatEnabled, 0) > 0.5f && coat > 0) {
					// the graph has HDRP's clear coat feature on; _Clearcoat (the player's paint setting) is the coat's strength
					d.clearCoat = Mathf.Clamp01(coat);
					d.clearCoatRoughness = 0.04f;
					flags |= AeConst.MaterialClearCoat;
				}
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				if (cls == AeConst.ClassVehicleStitches) {
					// the panel seams: a strip of paint over the body whose pack texture says how much of it shows (the
					// shader reads it from the mask slot; no mask layout: it is not a roughness / metallic map)
					scratch_[AeConst.SlotMask] = Tex(m, StitchesPack);
				}
				if (cls == AeConst.ClassVehiclePaint) {
					// _BaseMap of the body is the livery (vinyls, stickers) over the paint; the colours' alpha is not used by the
					// game (0 everywhere seen), so it carries "a livery texture is bound" to the shader
					d.baseColor[3] = scratch_[AeConst.SlotBase] != null ? 1.0f : 0.0f;
				}
				uvProperty = BaseMap;
			} else if (m.HasProperty(AlphaClippingThreshold) && m.HasProperty(Spectrum)) {
				cls = AeConst.ClassVehicleGlass;
				SetColor(d.baseColor, Col(m, BaseColor));
				d.roughness = 0.03f;
				d.alphaCutoff = m.GetFloat(AlphaClippingThreshold);
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				transparent = true;
				flags |= AeConst.MaterialDoubleSided;
			} else if (m.HasProperty(TireBaseMap)) {
				cls = AeConst.ClassVehicleTire;
				SetColor(d.baseColor, Color.white);
				scratch_[AeConst.SlotBase] = Tex(m, TireBaseMap);
				scratch_[AeConst.SlotMask] = Tex(m, TirePackMap);
				scratch_[AeConst.SlotNormal] = Tex(m, TireNormalMap);
				d.maskLayout = AeConst.MaskRmo;
				d.roughness = 1;
				d.metallic = 1;
				uvProperty = TireBaseMap;
			} else if (m.HasProperty(BaseMap) && m.HasProperty(PackMap)) {
				// the game's generic car graphs (CarGeneric, CarGenericAlpha, CarLights): base, pack (roughness / metallic /
				// occlusion), normal, and either a colour mask or emission masks
				cls = m.HasProperty(EmissionMasks) ? AeConst.ClassVehicleLights : AeConst.ClassVehicleGeneric;
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				scratch_[AeConst.SlotMask] = Tex(m, PackMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				d.maskLayout = AeConst.MaskRmo;
				d.roughness = 1;
				d.metallic = 1;
				if (cls == AeConst.ClassVehicleLights) {
					// CarLights: _EmissionMasks has one light group per channel, _EmissionIntensity (a vector) switches each
					// group on. The four intensities travel in secondaryColor; the emission is the lamp's own base colour.
					scratch_[AeConst.SlotEmissive] = Tex(m, EmissionMasks);
					Vector4 groups = Vec(m, EmissionIntensity);
					d.secondaryColor[0] = groups.x; d.secondaryColor[1] = groups.y; d.secondaryColor[2] = groups.z; d.secondaryColor[3] = groups.w;
					if (scratch_[AeConst.SlotEmissive] != null) {
						d.emissive[0] = d.emissive[1] = d.emissive[2] = 1;
					}
				}
				if (cls == AeConst.ClassVehicleGeneric) {
					scratch_[AeConst.SlotExtra] = Tex(m, ColormaskMap);
					// the colours tint through the mask; their alpha is the weight (0 in the materials seen = untinted)
					if (m.HasProperty(SecondaryColor)) {
						SetColor(d.secondaryColor, Col(m, SecondaryColor));
					}
					scratch_[AeConst.SlotEmissive] = Tex(m, EmissionMap);
					float intensity = Float(m, EmissionIntensity, 0);
					if (scratch_[AeConst.SlotEmissive] != null && intensity > 0) {
						d.emissive[0] = d.emissive[1] = d.emissive[2] = intensity;
					}
				}
				uvProperty = BaseMap;
			} else if (m.HasProperty(MaxBaseColorMap) && m.HasProperty(MaxMetalness)) {
				// HDRP's 3ds Max physical material
				cls = AeConst.ClassStandard;
				SetColor(d.baseColor, Col(m, MaxBaseColor));
				scratch_[AeConst.SlotBase] = Tex(m, MaxBaseColorMap);
				d.roughness = Mathf.Max(m.GetFloat(MaxRoughness), 0.05f);
				d.metallic = m.GetFloat(MaxMetalness);
				uvProperty = MaxBaseColorMap;
			} else if (m.HasProperty(LayerCount) && m.HasProperty(BaseColorMap0)) {
				// HDRP/LayeredLit (roads, terrain in maps made for HDRP): its main layer as HDRP/Lit is drawn, layers 1 .. 3
				// over it, blended by _LayerMaskMap (R, G, B for layers 1, 2, 3; with the vertex colour per _VertexColorMode).
				// Not reproduced: height based blending, layers inheriting the base layer's colour, planar / triplanar layer
				// mapping (the mesh's uv is used). (It has no _BaseColorMap: before this it fell to the _MainTex case below -
				// the lightmapper's hidden property, empty - and was drawn black.)
				cls = AeConst.ClassStandard;
				int layers = Mathf.Clamp(Mathf.RoundToInt(Float(m, LayerCount, 1)), 1, 4);
				if (layers > 1) {
					d.layerCount = (uint)layers;
					d.layerVertexColor = (uint)Mathf.Clamp(Mathf.RoundToInt(Float(m, VertexColorMode, 0)), 0, 2);
					r.layerTextures[0] = Tex(m, LayerMaskMap);
					Vector2 maskScale = m.GetTextureScale(LayerMaskMap), maskOffset = m.GetTextureOffset(LayerMaskMap);
					d.layerMaskUv[0] = maskScale.x;
					d.layerMaskUv[1] = maskScale.y;
					d.layerMaskUv[2] = maskOffset.x;
					d.layerMaskUv[3] = maskOffset.y;
					for (int l = 0; l < layers - 1; ++l) {
						Color c = Col(m, LayerBaseColor[l]);
						d.layerColor[l * 4] = c.r;
						d.layerColor[l * 4 + 1] = c.g;
						d.layerColor[l * 4 + 2] = c.b;
						d.layerColor[l * 4 + 3] = c.a;
						Texture baseTex = Tex(m, LayerBaseColorMap[l]), maskTex = Tex(m, LayerMaskMapIds[l]);
						r.layerTextures[1 + l * 3] = baseTex;
						r.layerTextures[2 + l * 3] = Tex(m, LayerNormalMap[l]);
						r.layerTextures[3 + l * 3] = maskTex;
						d.layerParams[l * 4] = maskTex != null ? Float(m, LayerMetallicRemapMax[l], 1) : Float(m, LayerMetallic[l], 0);
						d.layerParams[l * 4 + 1] = 1.0f - (maskTex != null ? Float(m, LayerSmoothnessRemapMax[l], 1) : Float(m, LayerSmoothness[l], 0.5f));
						d.layerParams[l * 4 + 2] = Float(m, LayerNormalScale[l], 1);
						d.layerParams[l * 4 + 3] = maskTex != null ? AeConst.MaskHdrp : 0;
						Vector2 scale = m.HasProperty(LayerBaseColorMap[l]) ? m.GetTextureScale(LayerBaseColorMap[l]) : Vector2.one;
						Vector2 offset = m.HasProperty(LayerBaseColorMap[l]) ? m.GetTextureOffset(LayerBaseColorMap[l]) : Vector2.zero;
						d.layerUv[l * 4] = scale.x;
						d.layerUv[l * 4 + 1] = scale.y;
						d.layerUv[l * 4 + 2] = offset.x;
						d.layerUv[l * 4 + 3] = offset.y;
					}
				}
				SetColor(d.baseColor, Col(m, BaseColor0));
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap0);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap0);
				scratch_[AeConst.SlotMask] = Tex(m, MaskMap0);
				d.normalScale = Float(m, NormalScale0, 1);
				if (scratch_[AeConst.SlotMask] != null) {
					d.maskLayout = AeConst.MaskHdrp;
					d.metallic = Float(m, MetallicRemapMax0, 1);
					d.roughness = 1.0f - Float(m, SmoothnessRemapMax0, 1);
				} else {
					d.metallic = Float(m, Metallic0, 0);
					d.roughness = 1.0f - Float(m, Smoothness0, 0.5f);
				}
				d.alphaCutoff = Float(m, AlphaCutoff, 0.5f);
				uvProperty = BaseColorMap0;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(Metallic) && m.HasProperty(Smoothness)) {
				// HDRP/Lit
				cls = AeConst.ClassStandard;
				SetColor(d.baseColor, Col(m, BaseColor));
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				scratch_[AeConst.SlotMask] = Tex(m, MaskMap);
				d.normalScale = Float(m, NormalScale, 1);
				if (scratch_[AeConst.SlotMask] != null) {
					d.maskLayout = AeConst.MaskHdrp;
					d.metallic = Float(m, MetallicRemapMax, 1);
					d.roughness = 1.0f - Float(m, SmoothnessRemapMax, 1);
				} else {
					d.metallic = Float(m, Metallic, 0);
					d.roughness = 1.0f - Float(m, Smoothness, 0.5f);
				}
				if (m.HasProperty(EmissiveColor)) {
					Color e = m.GetColor(EmissiveColor); // an HDR colour: already linear, in nits
					d.emissive[0] = e.r;
					d.emissive[1] = e.g;
					d.emissive[2] = e.b;
					scratch_[AeConst.SlotEmissive] = Tex(m, EmissiveColorMap);
				}
				d.alphaCutoff = Float(m, AlphaCutoff, 0.5f);
				uvProperty = BaseColorMap;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(MaskMap) && m.HasProperty(BaseColor)) {
				// the game's Colormask graph: base map tinted by _BaseColor, normal, mask
				cls = AeConst.ClassColorMask;
				SetColor(d.baseColor, Col(m, BaseColor));
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				scratch_[AeConst.SlotMask] = Tex(m, MaskMap);
				if (scratch_[AeConst.SlotMask] != null) {
					d.maskLayout = AeConst.MaskHdrp;
					d.metallic = 1;
					d.roughness = 0;
				}
				uvProperty = BaseColorMap;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(DiffusionProfileHash)) {
				// the game's foliage graph: an alpha tested, double sided leaf card
				cls = AeConst.ClassVegetation;
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				d.roughness = 0.8f;
				uvProperty = BaseColorMap;
			} else if (m.shader.name.IndexOf("Blobshadow", StringComparison.OrdinalIgnoreCase) >= 0) {
				// the car's blob shadow quad (CarBlobshadow, an unlit transparent graph with only _BaseColorMap): the soft
				// dark patch the game puts under every car, on top of the real shadows. Drawn the way the game draws it.
				cls = AeConst.ClassBlobShadow;
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				transparent = true;
				flags |= AeConst.MaterialNoShadow;
			} else if (m.HasProperty(BaseColorMap) && transparent && !m.HasProperty(MainTex)) {
				// an unlit transparent graph with nothing but a texture, not known: not drawn
				cls = AeConst.ClassSkip;
			} else if (m.HasProperty(MainTex)) {
				// particles, video panels: unlit, their texture as it is
				cls = AeConst.ClassUnlit;
				scratch_[AeConst.SlotBase] = Tex(m, MainTex);
				if (!m.HasProperty(ShouldFlipX) && transparent) {
					flags |= AeConst.MaterialAdditive; // Mobile/Particles/Additive and the like
				}
				uvProperty = MainTex;
			}

			if (cls == AeConst.ClassUnsupported) {
				// A shader nothing above knows (a map maker's own Shader Graph): drawn as a plain surface with its main
				// texture rather than the magenta "unsupported" checker. (sadamine's trees and slopes, 616 materials of one
				// graph with a texture named "_main", were all checkered.) The texture: one whose name says it is the colour,
				// else the first one bound. Alpha tested and double sided as the material's HDRP flags say; an alpha tested,
				// double sided one is taken as foliage.
				int main = MainTextureGuess(m);
				if (main != 0) {
					bool foliage = (flags & AeConst.MaterialAlphaTest) != 0 && (flags & AeConst.MaterialDoubleSided) != 0;
					cls = foliage ? AeConst.ClassVegetation : AeConst.ClassStandard;
					scratch_[AeConst.SlotBase] = m.GetTexture(main);
					d.roughness = 0.8f;
					d.alphaCutoff = Float(m, AlphaCutoff, 0.5f);
					uvProperty = main;
				}
			}

			if (cls != AeConst.ClassUnsupported && cls != AeConst.ClassSkip) {
				if (transparent) {
					flags |= AeConst.MaterialBlend;
					if (cls == AeConst.ClassStandard) {
						cls = AeConst.ClassTransparent;
					}
				}
				if (uvProperty != 0 && m.HasProperty(uvProperty)) {
					Vector2 scale = m.GetTextureScale(uvProperty), offset = m.GetTextureOffset(uvProperty);
					d.uvScale[0] = scale.x;
					d.uvScale[1] = scale.y;
					d.uvOffset[0] = offset.x;
					d.uvOffset[1] = offset.y;
				}
				if (over_ != null && (over_.HasColor(BaseColor) || over_.HasVector(BaseColor)) && cls != AeConst.ClassVehiclePaint) {
					SetColor(d.baseColor, Col(m, BaseColor)); // (classes that take no colour from the material itself)
				}
			}
			// textures the material has that nothing above mapped: reported, so a gap in the translation is visible
			uint unmapped = 0;
			if (cls != AeConst.ClassSkip) {
				ids_.Clear();
				m.GetTexturePropertyNameIDs(ids_);
				foreach (int id in ids_) {
					Texture t = m.GetTexture(id);
					if (t == null) {
						continue;
					}
					bool used = false;
					for (int i = 0; i < 5; ++i) {
						used |= scratch_[i] == t;
					}
					foreach (Texture layer in r.layerTextures) {
						used |= layer == t;
					}
					if (!used) {
						++unmapped;
					}
				}
			}
			d.unsupportedProperties = unmapped;
			d.materialClass = cls;
			d.flags = flags;
			for (int i = 0; i < 5; ++i) {
				r.textures[i] = scratch_[i];
			}
			r.desc = d;
			return r;
		}

		private static readonly List<int> ids_ = new List<int>();
		private static readonly string[] colourNames_ = { "main", "albedo", "basecolor", "base", "diffuse", "color", "colour", "tex" };

		/// <summary>An unknown shader's colour texture: the property whose name says so (in that order of preference), else
		/// the first 2D texture bound. 0: none.</summary>
		private static int MainTextureGuess(Material m) {
			string[] names = m.GetTexturePropertyNames();
			foreach (string want in colourNames_) {
				foreach (string n in names) {
					if (n.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0 && n.IndexOf("normal", StringComparison.OrdinalIgnoreCase) < 0 && n.IndexOf("mask", StringComparison.OrdinalIgnoreCase) < 0 &&
						m.GetTexture(n) is Texture2D) {
						return Shader.PropertyToID(n);
					}
				}
			}
			foreach (string n in names) {
				if (m.GetTexture(n) is Texture2D && n.IndexOf("normal", StringComparison.OrdinalIgnoreCase) < 0 && !n.StartsWith("unity_", StringComparison.Ordinal)) {
					return Shader.PropertyToID(n);
				}
			}
			return 0;
		}
	}
}
