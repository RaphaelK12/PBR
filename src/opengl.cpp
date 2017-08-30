/*
 * Physically Based Rendering
 * Copyright (c) 2017 Michał Siejak
 */

#include <stdexcept>
#include <memory>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "common/mesh.hpp"
#include "common/image.hpp"
#include "common/utils.hpp"
#include "opengl.hpp"

namespace OpenGL {

enum UniformLocations : GLuint
{
	ViewProjectionMatrix = 0,
	EyePosition = 1,
	SpecularMapRoughness = 0,
};

GLFWwindow* Renderer::initialize(int width, int height, int samples)
{
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#if _DEBUG
	glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

	glfwWindowHint(GLFW_DEPTH_BITS, 0);
	glfwWindowHint(GLFW_STENCIL_BITS, 0);
	glfwWindowHint(GLFW_SAMPLES, 0);

	GLFWwindow* window = glfwCreateWindow(width, height, "Physically Based Rendering (OpenGL 4.5)", nullptr, nullptr);
	if(!window) {
		throw std::runtime_error("Failed to create OpenGL context");
	}

	glfwMakeContextCurrent(window);
	glfwSwapInterval(-1);

	if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		throw std::runtime_error("Failed to initialize OpenGL extensions loader");
	}

#if _DEBUG
	glDebugMessageCallback(Renderer::logMessage, nullptr);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
#endif

	m_framebuffer = createFrameBuffer(width, height, samples, GL_RGBA16F, GL_DEPTH24_STENCIL8);
	if(samples > 0) {
		m_resolveFramebuffer = createFrameBuffer(width, height, 0, GL_RGBA16F, GL_NONE);
	}
	else {
		m_resolveFramebuffer = m_framebuffer;
	}

	return window;
}

void Renderer::shutdown()
{
	if(m_framebuffer.id != m_resolveFramebuffer.id) {
		deleteFrameBuffer(m_resolveFramebuffer);
	}
	deleteFrameBuffer(m_framebuffer);

	deleteVertexBuffer(m_screenQuad);
	deleteVertexBuffer(m_skybox);
	deleteVertexBuffer(m_pbrModel);
	
	glDeleteProgram(m_tonemapProgram);
	glDeleteProgram(m_skyboxProgram);
	glDeleteProgram(m_pbrProgram);

	deleteTexture(m_envTexture);
	deleteTexture(m_irmapTexture);
	deleteTexture(m_spmapTexture);
	deleteTexture(m_spBRDF_LUT);

	deleteTexture(m_albedoTexture);
	deleteTexture(m_normalTexture);
	deleteTexture(m_metalnessTexture);
	deleteTexture(m_roughnessTexture);
}

void Renderer::setup()
{
	// Set OpenGL state.
	glEnable(GL_CULL_FACE);
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	glFrontFace(GL_CCW);

	// Load assets & compile/link rendering programs.
	m_screenQuad = createClipSpaceQuad();
	m_tonemapProgram = linkProgram({
		compileShader("shaders/glsl/passthrough_vs.glsl", GL_VERTEX_SHADER),
		compileShader("shaders/glsl/tonemap_fs.glsl", GL_FRAGMENT_SHADER)
	});

	m_skybox = createVertexBuffer(Mesh::fromFile("meshes/skybox.obj"));
	m_skyboxProgram = linkProgram({
		compileShader("shaders/glsl/skybox_vs.glsl", GL_VERTEX_SHADER),
		compileShader("shaders/glsl/skybox_fs.glsl", GL_FRAGMENT_SHADER)
	});

	m_pbrModel = createVertexBuffer(Mesh::fromFile("meshes/cerberus.fbx"));
	m_pbrProgram = linkProgram({
		compileShader("shaders/glsl/pbr_vs.glsl", GL_VERTEX_SHADER),
		compileShader("shaders/glsl/pbr_fs.glsl", GL_FRAGMENT_SHADER)
	});

	m_albedoTexture = createTexture(Image::fromFile("textures/cerberus_A.png", 3), GL_RGB, GL_SRGB8);
	m_normalTexture = createTexture(Image::fromFile("textures/cerberus_N.png", 3), GL_RGB, GL_RGB8);
	m_metalnessTexture = createTexture(Image::fromFile("textures/cerberus_M.png", 1), GL_RED, GL_R8);
	m_roughnessTexture = createTexture(Image::fromFile("textures/cerberus_R.png", 1), GL_RED, GL_R8);
	
	// Convert equirectangular environment map to a cubemap texture.
	{
		GLuint equirectToCubeProgram = linkProgram({
			compileShader("shaders/glsl/equirect2cube_cs.glsl", GL_COMPUTE_SHADER)
		});

		Texture envTextureEquirect = createTexture(Image::fromFile("environment.hdr", 3), GL_RGB, GL_RGB16F, 1);
		m_envTexture = createTexture(GL_TEXTURE_CUBE_MAP, 1024, 1024, GL_RGBA16F);

		glUseProgram(equirectToCubeProgram);
		glBindTextureUnit(0, envTextureEquirect.id);
		glBindImageTexture(0, m_envTexture.id, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		glDispatchCompute(m_envTexture.width/32, m_envTexture.height/32, 6);
		
		glDeleteTextures(1, &envTextureEquirect.id);
		glDeleteProgram(equirectToCubeProgram);

		glGenerateTextureMipmap(m_envTexture.id);
	}

	// Compute irradiance cubemap.
	{
		GLuint irmapProgram = linkProgram({
			compileShader("shaders/glsl/irmap_cs.glsl", GL_COMPUTE_SHADER)
		});

		m_irmapTexture = createTexture(GL_TEXTURE_CUBE_MAP, 32, 32, GL_RGBA16F, 1);

		glUseProgram(irmapProgram);
		glBindTextureUnit(0, m_envTexture.id);
		glBindImageTexture(0, m_irmapTexture.id, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
		glDispatchCompute(m_irmapTexture.width/32, m_irmapTexture.height/32, 6);
		glDeleteProgram(irmapProgram);
	}

	// Compute pre-filtered specular cubemap
	{
		GLuint spmapProgram = linkProgram({
			compileShader("shaders/glsl/spmap_cs.glsl", GL_COMPUTE_SHADER)
		});

		m_spmapTexture = createTexture(GL_TEXTURE_CUBE_MAP, 1024, 1024, GL_RGBA16F);
		const float deltaRoughness = 1.0f / glm::max(float(m_spmapTexture.levels-1), 1.0f);

		glUseProgram(spmapProgram);
		glBindTextureUnit(0, m_envTexture.id);
		for(int level=0, size=1024; level<=m_spmapTexture.levels; ++level, size/=2) {
			const GLuint numGroups = glm::max(1, size/32);
			glBindImageTexture(0, m_spmapTexture.id, level, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
			glProgramUniform1f(spmapProgram, SpecularMapRoughness, level * deltaRoughness);
			glDispatchCompute(numGroups, numGroups, 6);
		}
		glDeleteProgram(spmapProgram);
	}

	// Compute Cook-Torrance BRDF 2D LUT for split-sum approximation
	{
		GLuint spBRDFProgram = linkProgram({
			compileShader("shaders/glsl/spbrdf_cs.glsl", GL_COMPUTE_SHADER)
		});

		m_spBRDF_LUT = createTexture(GL_TEXTURE_2D, 256, 256, GL_RG16F, 1);

		glUseProgram(spBRDFProgram);
		glBindImageTexture(0, m_spBRDF_LUT.id, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
		glDispatchCompute(m_spBRDF_LUT.width/32, m_spBRDF_LUT.height/32, 1);
		glDeleteProgram(spBRDFProgram);
	}

	glFinish();
}

void Renderer::render(GLFWwindow* window, const ViewSettings& view)
{
	const glm::mat4 projMatrix     = glm::perspectiveFov(view.fov, float(m_framebuffer.width), float(m_framebuffer.height), 1.0f, 1000.0f);
	const glm::mat4 rotationMatrix = glm::eulerAngleXY(glm::radians(view.pitch), glm::radians(view.yaw));
	const glm::mat4 viewMatrix     = glm::translate(glm::mat4(), {0.0f, 0.0f, -view.distance}) * rotationMatrix;
	const glm::vec3 eyePosition    = glm::inverse(viewMatrix)[3];

	// Set skybox program uniforms.
	glProgramUniformMatrix4fv(m_skyboxProgram, ViewProjectionMatrix, 1, GL_FALSE, glm::value_ptr(projMatrix * rotationMatrix));

	// Set PBR program uniforms.
	glProgramUniformMatrix4fv(m_pbrProgram, ViewProjectionMatrix, 1, GL_FALSE, glm::value_ptr(projMatrix * viewMatrix));
	glProgramUniform3fv(m_pbrProgram, EyePosition, 1, glm::value_ptr(eyePosition));

	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer.id);
	glClear(GL_DEPTH_BUFFER_BIT); // No need to clear color, since we'll overwrite the screen with our skybox.

	// Draw skybox.
	glDisable(GL_DEPTH_TEST);
	glUseProgram(m_skyboxProgram);
	glBindTextureUnit(0, m_envTexture.id);
	glBindVertexArray(m_skybox.vao);
	glDrawElements(GL_TRIANGLES, m_skybox.numElements, GL_UNSIGNED_INT, 0);

	// Draw model.
	glEnable(GL_DEPTH_TEST);
	glUseProgram(m_pbrProgram);
	glBindTextureUnit(0, m_albedoTexture.id);
	glBindTextureUnit(1, m_normalTexture.id);
	glBindTextureUnit(2, m_metalnessTexture.id);
	glBindTextureUnit(3, m_roughnessTexture.id);
	glBindTextureUnit(4, m_irmapTexture.id);
	glBindTextureUnit(5, m_spmapTexture.id);
	glBindTextureUnit(6, m_spBRDF_LUT.id);
	glBindVertexArray(m_pbrModel.vao);
	glDrawElements(GL_TRIANGLES, m_pbrModel.numElements, GL_UNSIGNED_INT, 0);
		
	// Resolve multisample framebuffer.
	resolveFramebuffer(m_framebuffer, m_resolveFramebuffer);

	// Draw to window viewport (with tonemapping and gamma correction).
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(m_tonemapProgram);
	glBindTextureUnit(0, m_resolveFramebuffer.colorTarget);
	glBindVertexArray(m_screenQuad.vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glfwSwapBuffers(window);
}
	
GLuint Renderer::compileShader(const std::string& filename, GLenum type)
{
	const std::string src = File::readText(filename);
	if(src.empty()) {
		throw std::runtime_error("Cannot read shader source file: " + filename);
	}
	const GLchar* srcBufferPtr = src.c_str();

	std::printf("Compiling GLSL shader: %s\n", filename.c_str());

	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &srcBufferPtr, nullptr);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if(status != GL_TRUE) {
		GLsizei infoLogSize;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogSize);
		std::unique_ptr<GLchar[]> infoLog(new GLchar[infoLogSize]);
		glGetShaderInfoLog(shader, infoLogSize, nullptr, infoLog.get());
		throw std::runtime_error(std::string("Shader compilation failed: ") + filename + "\n" + infoLog.get());
	}
	return shader;
}
	
GLuint Renderer::linkProgram(std::initializer_list<GLuint> shaders)
{
	GLuint program = glCreateProgram();

	for(GLuint shader : shaders) {
		glAttachShader(program, shader);
	}
	glLinkProgram(program);
	for(GLuint shader : shaders) {
		glDetachShader(program, shader);
		glDeleteShader(shader);
	}

	GLint status;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if(status == GL_TRUE) {
		glValidateProgram(program);
		glGetProgramiv(program, GL_VALIDATE_STATUS, &status);
	}
	if(status != GL_TRUE) {
		GLsizei infoLogSize;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogSize);
		std::unique_ptr<GLchar[]> infoLog(new GLchar[infoLogSize]);
		glGetProgramInfoLog(program, infoLogSize, nullptr, infoLog.get());
		throw std::runtime_error(std::string("Program link failed\n") + infoLog.get());
	}
	return program;
}
	
Texture Renderer::createTexture(GLenum target, int width, int height, GLenum internalformat, int levels)
{
	Texture texture;
	texture.width  = width;
	texture.height = height;
	texture.levels = levels;
	
	if(texture.levels <= 0) {
		texture.levels = 1;
		while((width|height) >> texture.levels) {
			++texture.levels;
		}
	}
	
	glCreateTextures(target, 1, &texture.id);
	glTextureStorage2D(texture.id, texture.levels, internalformat, width, height);
	glTextureParameteri(texture.id, GL_TEXTURE_MIN_FILTER, texture.levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTextureParameteri(texture.id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	return texture;
}
	
Texture Renderer::createTexture(const std::shared_ptr<class Image>& image, GLenum format, GLenum internalformat, int levels)
{
	Texture texture = createTexture(GL_TEXTURE_2D, image->width(), image->height(), internalformat, levels);
	if(image->isHDR()) {
		glTextureSubImage2D(texture.id, 0, 0, 0, texture.width, texture.height, format, GL_FLOAT, image->pixels<float>());
	}
	else {
		glTextureSubImage2D(texture.id, 0, 0, 0, texture.width, texture.height, format, GL_UNSIGNED_BYTE, image->pixels<unsigned char>());
	}

	if(texture.levels > 1) {
		glGenerateTextureMipmap(texture.id);
	}
	return texture;
}
	
void Renderer::deleteTexture(Texture& texture)
{
	glDeleteTextures(1, &texture.id);
	std::memset(&texture, 0, sizeof(Texture));
}

FrameBuffer Renderer::createFrameBuffer(int width, int height, int samples, GLenum colorFormat, GLenum depthstencilFormat)
{
	FrameBuffer fb;
	fb.width   = width;
	fb.height  = height;
	fb.samples = samples;

	glCreateFramebuffers(1, &fb.id);

	if(colorFormat != GL_NONE) {
		if(samples > 0) {
			glCreateRenderbuffers(1, &fb.colorTarget);
			glNamedRenderbufferStorageMultisample(fb.colorTarget, samples, colorFormat, width, height);
			glNamedFramebufferRenderbuffer(fb.id, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb.colorTarget);
		}
		else {
			glCreateTextures(GL_TEXTURE_2D, 1, &fb.colorTarget);
			glTextureStorage2D(fb.colorTarget, 1, colorFormat, width, height);
			glNamedFramebufferTexture(fb.id, GL_COLOR_ATTACHMENT0, fb.colorTarget, 0);
		}
	}
	if(depthstencilFormat != GL_NONE) {
		glCreateRenderbuffers(1, &fb.depthStencilTarget);
		if(samples > 0) {
			glNamedRenderbufferStorageMultisample(fb.depthStencilTarget, samples, depthstencilFormat, width, height);
		}
		else {
			glNamedRenderbufferStorage(fb.depthStencilTarget, depthstencilFormat, width, height);
		}
		glNamedFramebufferRenderbuffer(fb.id, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb.depthStencilTarget);
	}

	GLenum status = glCheckNamedFramebufferStatus(fb.id, GL_DRAW_FRAMEBUFFER);
	if(status != GL_FRAMEBUFFER_COMPLETE) {
		throw std::runtime_error("Framebuffer completeness check failed: " + std::to_string(status));
	}
	return fb;
}

void Renderer::resolveFramebuffer(const FrameBuffer& srcfb, const FrameBuffer& dstfb)
{
	if(srcfb.id == dstfb.id) {
		return;
	}

	std::vector<GLenum> attachments;
	if(srcfb.colorTarget) {
		attachments.push_back(GL_COLOR_ATTACHMENT0);
	}
	if(srcfb.depthStencilTarget) {
		attachments.push_back(GL_DEPTH_STENCIL_ATTACHMENT);
	}
	assert(attachments.size() > 0);

	glBlitNamedFramebuffer(srcfb.id, dstfb.id, 0, 0, srcfb.width-1, srcfb.height-1, 0, 0, dstfb.width-1, dstfb.height-1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glInvalidateNamedFramebufferData(srcfb.id, (GLsizei)attachments.size(), &attachments[0]);
}
	
void Renderer::deleteFrameBuffer(FrameBuffer& fb)
{
	if(fb.id) {
		glDeleteFramebuffers(1, &fb.id);
	}
	if(fb.colorTarget) {
		if(fb.samples == 0) {
			glDeleteTextures(1, &fb.colorTarget);
		}
		else {
			glDeleteRenderbuffers(1, &fb.colorTarget);
		}
	}
	if(fb.depthStencilTarget) {
		glDeleteRenderbuffers(1, &fb.depthStencilTarget);
	}
	std::memset(&fb, 0, sizeof(FrameBuffer));
}

VertexBuffer Renderer::createVertexBuffer(const std::shared_ptr<class Mesh>& mesh)
{
	VertexBuffer buffer;
	buffer.numElements = static_cast<GLuint>(mesh->faces().size()) * 3;

	const size_t vertexDataSize = mesh->vertices().size() * sizeof(Mesh::Vertex);
	const size_t indexDataSize  = mesh->faces().size() * sizeof(Mesh::Face);

	glCreateBuffers(1, &buffer.vbo);
	glNamedBufferStorage(buffer.vbo, vertexDataSize, reinterpret_cast<const void*>(&mesh->vertices()[0]), 0);
	glCreateBuffers(1, &buffer.ibo);
	glNamedBufferStorage(buffer.ibo, indexDataSize, reinterpret_cast<const void*>(&mesh->faces()[0]), 0);

	glCreateVertexArrays(1, &buffer.vao);
	glVertexArrayElementBuffer(buffer.vao, buffer.ibo);
	for(int i=0; i<Mesh::NumAttributes; ++i) {
		glVertexArrayVertexBuffer(buffer.vao, i, buffer.vbo, i * sizeof(glm::vec3), sizeof(Mesh::Vertex));
		glEnableVertexArrayAttrib(buffer.vao, i);
		glVertexArrayAttribFormat(buffer.vao, i, i==(Mesh::NumAttributes-1) ? 2 : 3, GL_FLOAT, GL_FALSE, 0);
		glVertexArrayAttribBinding(buffer.vao, i, i);
	}
	return buffer;
}
	
VertexBuffer Renderer::createClipSpaceQuad()
{
	static const GLfloat vertices[] = {
		 1.0f,  1.0f, 1.0f, 1.0f,
	    -1.0f,  1.0f, 0.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,
	};

	VertexBuffer buffer;
	glCreateBuffers(1, &buffer.vbo);
	glNamedBufferStorage(buffer.vbo, sizeof(vertices), vertices, 0);

	glCreateVertexArrays(1, &buffer.vao);
	for(int i=0; i<2; ++i) {
		glVertexArrayVertexBuffer(buffer.vao, i, buffer.vbo, i * 2 * sizeof(GLfloat), 4 * sizeof(GLfloat));
		glEnableVertexArrayAttrib(buffer.vao, i);
		glVertexArrayAttribFormat(buffer.vao, i, 2, GL_FLOAT, GL_FALSE, 0);
		glVertexArrayAttribBinding(buffer.vao, i, i);
	}
	return buffer;
}

void Renderer::deleteVertexBuffer(VertexBuffer& buffer)
{
	if(buffer.vao) {
		glDeleteVertexArrays(1, &buffer.vao);
	}
	if(buffer.vbo) {
		glDeleteBuffers(1, &buffer.vbo);
	}
	if(buffer.ibo) {
		glDeleteBuffers(1, &buffer.ibo);
	}
	std::memset(&buffer, 0, sizeof(VertexBuffer));
}

#if _DEBUG
void Renderer::logMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
	if(severity != GL_DEBUG_SEVERITY_NOTIFICATION) {
		std::fprintf(stderr, "GL: %s\n", message);
	}
}
#endif

} // OpenGL