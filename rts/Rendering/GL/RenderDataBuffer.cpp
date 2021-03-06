/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>
#include <cstring> // memcpy

#include "myGL.h"
#include "RenderDataBuffer.hpp"

void GL::RenderDataBuffer::EnableAttribs(size_t numAttrs, const Shader::ShaderInput* rawAttrs) const {
	for (size_t n = 0; n < numAttrs; n++) {
		const Shader::ShaderInput& a = rawAttrs[n];

		glEnableVertexAttribArray(a.index);
		glVertexAttribPointer(a.index, a.count, a.type, false, a.stride, a.data);
	}
}

void GL::RenderDataBuffer::DisableAttribs(size_t numAttrs, const Shader::ShaderInput* rawAttrs) const {
	for (size_t n = 0; n < numAttrs; n++) {
		glDisableVertexAttribArray(rawAttrs[n].index);
	}
}


char* GL::RenderDataBuffer::FormatShaderBase(
	char* buf,
	const char* end,
	const char* defines,
	const char* globals,
	const char* type,
	const char* name
) {
	std::memset(buf, 0, (end - buf));

	char* ptr = &buf[0];
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "#version 410 core\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "#extension GL_ARB_explicit_attrib_location : enable\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "// defines\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "#define VA_TYPE %s\n", name);
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", defines);
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "// globals\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", globals);
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "// uniforms\n");

	switch (type[0]) {
		case 'V': {
			ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "uniform mat4 u_movi_mat;\n");
			ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "uniform mat4 u_proj_mat;\n");
		} break;
		case 'F': {
			ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "uniform sampler2D u_tex0;\n"); // T*,2DT* (v_texcoor_st)
			ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "uniform sampler3D u_tex1;\n"); // TNT (v_texcoor_uv1)
			ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "uniform sampler3D u_tex2;\n"); // TNT (v_texcoor_uv2)
		} break;
		default: {} break;
	}

	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\n");
	return ptr;
}

char* GL::RenderDataBuffer::FormatShaderType(
	char* buf,
	char* ptr,
	const char* end,
	size_t numAttrs,
	const Shader::ShaderInput* rawAttrs,
	const char* code,
	const char* type,
	const char* name
) {
	constexpr const char* vecTypes[] = {"vec2", "vec3", "vec4"};
	constexpr const char* vsInpFmt = "layout(location = %d) in %s %s;\n";
	constexpr const char* vsOutFmt = "out %s v_%s;\n"; // prefix VS outs by "v_"
	constexpr const char* fsInpFmt = "in %s v_%s;\n";
	constexpr const char* fsOutFmt = "layout(location = 0) out vec4 f_%s;\n"; // prefix (single fixed) FS out by "f_"

	{
		ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "// %s input attributes\n", type);

		for (size_t n = 0; n < numAttrs; n++) {
			const Shader::ShaderInput& a = rawAttrs[n];

			assert(a.count >= 2);
			assert(a.count <= 4);

			switch (type[0]) {
				case 'V': { ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), vsInpFmt, a.index, vecTypes[a.count - 2], a.name); } break;
				case 'F': { ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), fsInpFmt, vecTypes[a.count - 2], a.name + 2); } break;
				default: {} break;
			}
		}
	}

	{
		ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "// %s output attributes\n", type);

		switch (type[0]) {
			case 'V': {
				for (size_t n = 0; n < numAttrs; n++) {
					const Shader::ShaderInput& a = rawAttrs[n];

					assert(a.name[0] == 'a');
					assert(a.name[1] == '_');

					ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), vsOutFmt, vecTypes[a.count - 2], a.name + 2);
				}
			} break;
			case 'F': {
				ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), fsOutFmt, "color_rgba");
			} break;
			default: {} break;
		}
	}

	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\n");
	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "void main() {\n");

	if (code[0] != '\0') {
		ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s\n", code);
	} else {
		switch (type[0]) {
			case 'V': {
				// position (2D or 3D) is always the first attribute
				switch (rawAttrs[0].count) {
					case 2: { ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\tgl_Position = u_proj_mat * u_movi_mat * vec4(a_vertex_xy , 0.0, 1.0);\n"); } break;
					case 3: { ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\tgl_Position = u_proj_mat * u_movi_mat * vec4(a_vertex_xyz,      1.0);\n"); } break;
					default: {} break;
				}

				for (size_t n = 1; n < numAttrs; n++) {
					const Shader::ShaderInput& a = rawAttrs[n];

					// assume standard tc-gen
					if (std::strcmp(a.name, "a_texcoor_st") == 0) {
						ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "\tv_texcoor_st = a_texcoor_st;\n");
						continue;
					}
					if (std::strcmp(a.name, "a_texcoor_uv") == 0) {
						ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "\tv_texcoor_uv%d = a_texcoor_uv%d;\n", a.name[12] - '0', a.name[12] - '0');
						continue;
					}
				}
			} break;
			case 'F': {} break;
			default: {} break;
		}
	}

	ptr += std::snprintf(ptr, (end - buf) - (ptr - buf), "%s", "}\n");
	return ptr;
}


void GL::RenderDataBuffer::CreateShader(
	size_t numObjects,
	size_t numUniforms,
	Shader::GLSLShaderObject* objects,
	const Shader::ShaderInput* uniforms
) {
	for (size_t n = 0; n < numObjects; n++) {
		shader.AttachShaderObject(&objects[n]);
	}

	shader.ReloadShaderObjects();
	shader.CreateAndLink();
	shader.RecalculateShaderHash();

	for (size_t n = 0; n < numUniforms; n++) {
		shader.SetUniform(uniforms[n]);
	}
}


void GL::RenderDataBuffer::Upload(
	size_t numElems,
	size_t numIndcs,
	size_t numAttrs,
	const uint8_t* rawElems,
	const uint8_t* rawIndcs,
	const Shader::ShaderInput* rawAttrs
) {
	array.Bind();
	elems.Bind();
	indcs.Bind();
	elems.New(numElems * sizeof(uint8_t), GL_STATIC_DRAW, rawElems);
	indcs.New(numIndcs * sizeof(uint8_t), GL_STATIC_DRAW, rawIndcs);

	EnableAttribs(numAttrs, rawAttrs);
	array.Unbind();
	elems.Unbind();
	indcs.Unbind();
	DisableAttribs(numAttrs, rawAttrs);
}


void GL::RenderDataBuffer::Submit(uint32_t primType, uint32_t dataSize, uint32_t dataType) {
	array.Bind();

	if (indcs.bufSize == 0) {
		// dataSize := numElems (unique verts)
		glDrawArrays(primType, 0, dataSize);
	} else {
		// dataSize := numIndcs, dataType := GL_UNSIGNED_INT
		assert(dataType == GL_UNSIGNED_INT);
		glDrawElements(primType, dataSize, dataType, nullptr);
	}

	array.Unbind();
}




#if 0
void GL::RenderDataBuffer::UploadC(
	size_t numElems,
	size_t numIndcs,
	const VA_TYPE_C* rawElems,
	const uint32_t* rawIndcs
) {
	uploadBuffer.clear();
	uploadBuffer.reserve(numElems * (VA_SIZE_C + 3));

	for (size_t n = 0; n < numElems; n++) {
		const VA_TYPE_C& e = rawElems[n];

		uploadBuffer.push_back(e.p.x);
		uploadBuffer.push_back(e.p.y);
		uploadBuffer.push_back(e.p.z);
		uploadBuffer.push_back(e.c.r); // turn SColor uint32 into 4 floats
		uploadBuffer.push_back(e.c.g);
		uploadBuffer.push_back(e.c.b);
		uploadBuffer.push_back(e.c.a);
	}

	Upload(uploadBuffer.size(), numIndcs, NUM_VA_TYPE_C_ATTRS, uploadBuffer.data(), rawIndcs, VA_TYPE_C_ATTRS);
}
#endif

