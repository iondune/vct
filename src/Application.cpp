#include "Application.h"

#include <Graphics/opengl.h>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>

#include <iostream>
#include <vector>
#include <memory>

#include "Graphics/GLHelper.h"
#include "Graphics/GLShaderProgram.h"
#include "Graphics/GLQuad.h"
#include "Graphics/GLTimer.h"

#include "Input/Keyboard.h"
#include "Input/Mouse.h"

#include "Overlay.h"
#include "Camera.h"
#include "Scene.h"

#include "common.h"

#define SHADOWMAP_WIDTH 4096
#define SHADOWMAP_HEIGHT 4096

GLuint make3DTexture(GLsizei size, GLsizei levels, GLenum internalFormat, GLint minFilter, GLint magFilter);

void view2DTexture(GLuint texture);

void Application::init() {
	// Setup for OpenGL
	glfwGetFramebufferSize(window, &width, &height);
	glViewport(0, 0, width, height);
	glClearColor(0.5294f, 0.8078f, 0.9216f, 1.0f);
	
	// Setup framebuffers
	shadowmapFBO.bind();
	glm::vec4 borderColor{ 1.0f };
	shadowmapFBO.attachTexture(
		GL_DEPTH_ATTACHMENT, GL_DEPTH_COMPONENT,
		SHADOWMAP_WIDTH, SHADOWMAP_HEIGHT,
		GL_DEPTH_COMPONENT, GL_FLOAT,
		GL_LINEAR, GL_LINEAR,
		GL_CLAMP_TO_BORDER, GL_CLAMP_TO_BORDER,
		&borderColor
	);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	if (shadowmapFBO.getStatus() != GL_FRAMEBUFFER_COMPLETE) {
		LOG_ERROR("shadowmapFBO not created successfully");
	}
	shadowmapFBO.unbind();

	// Create shaders
	program.attachAndLink({SHADER_DIR "phong.vert", SHADER_DIR "phong.frag"});
	program.setObjectLabel("Phong");
	voxelProgram.attachAndLink({SHADER_DIR "voxelize.vert", SHADER_DIR "voxelize.frag", SHADER_DIR "voxelize.geom"});
	voxelProgram.setObjectLabel("Voxelize");
	shadowmapProgram.attachAndLink({SHADER_DIR "simple.vert", SHADER_DIR "empty.frag"});
	shadowmapProgram.setObjectLabel("Shadowmap");
	injectRadianceProgram.attachAndLink({SHADER_DIR "injectRadiance.comp"});
	injectRadianceProgram.setObjectLabel("Inject Radiance");
	mipmapProgram.attachAndLink({SHADER_DIR "filterRadiance.comp"});
	mipmapProgram.setObjectLabel("Filter Radiance");

	// Initialize voxel textures
	useRGBA16f = GLAD_GL_NV_shader_atomic_fp16_vector;
	GLenum voxelFormat = useRGBA16f ? GL_RGBA16F : GL_RGBA8;
	// voxelColor = make3DTexture(voxelDim, 4, voxelFormat, GL_LINEAR_MIPMAP_LINEAR, GL_NEAREST);
	voxelColor = make3DTexture(voxelDim, 1, voxelFormat, GL_LINEAR, GL_NEAREST);
	voxelNormal = make3DTexture(voxelDim, 1, voxelFormat, GL_NEAREST, GL_NEAREST);
	voxelRadiance = make3DTexture(voxelDim, voxelLevels, GL_RGBA8, GL_LINEAR_MIPMAP_LINEAR, GL_NEAREST);

	// Create scene
	scene = std::make_unique<Scene>();
	scene->addMesh(RESOURCE_DIR "sponza2/sponza.obj", glm::scale(glm::mat4(1.0f), glm::vec3(0.01f)));
	scene->addMesh(RESOURCE_DIR "nanosuit/nanosuit.obj", glm::scale(glm::mat4(1.0f), glm::vec3(0.25f)));
	// scene->addMesh(RESOURCE_DIR "cube.obj");
	// scene->addMesh(RESOURCE_DIR "cube.obj", glm::translate(glm::scale(glm::mat4(), glm::vec3(5)), glm::vec3(0,-2,0)));
	// scene->addMesh(RESOURCE_DIR "sphere.obj", glm::translate(glm::mat4(), glm::vec3(2.5,0,0)));
	scene->setMainlight({12.0f, 40.0f, -7.0f}, {-0.38f, -0.88f, 0.2f}, {1.0f, 1.0f, 1.0f});

	// Camera setup
	camera.position = glm::vec3(5, 1, 0);
	camera.yaw = 180.0f;
	camera.update(0.0f);

    GLQuad::init();
}

void Application::update(float dt) {
	Mouse::update();

	if (GLFW_PRESS == Keyboard::getKey(GLFW_KEY_ESCAPE)) {
		glfwSetWindowShouldClose(window, 1);
	}

	if (Keyboard::getKeyTap(GLFW_KEY_LEFT_CONTROL)) {
		int mode = glfwGetInputMode(window, GLFW_CURSOR);
		glfwSetInputMode(window, GLFW_CURSOR, (mode == GLFW_CURSOR_DISABLED) ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
	}

	if (Keyboard::getKeyTap(GLFW_KEY_GRAVE_ACCENT)) {
		ui.enabled = !ui.enabled;
	}

	if (Keyboard::getKeyTap(GLFW_KEY_V)) {
		settings.drawVoxels = !settings.drawVoxels;
	}

	if (GLFW_CURSOR_DISABLED == glfwGetInputMode(window, GLFW_CURSOR)) {
		camera.update(dt);
	}
}

void Application::render(float dt) {
	totalTimer.start();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	Light mainlight = scene->getMainlight();
	
	voxelizeTimer.start();
	// Voxelize scene
	{
		GL_DEBUG_PUSH("Voxelize Scene")
		glViewport(0, 0, voxelDim, voxelDim);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDepthMask(GL_FALSE);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		if (GLAD_GL_NV_conservative_raster && settings.conservativeRasterization) {
			glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
		}

		glClearTexImage(voxelColor, 0, GL_RGBA, GL_FLOAT, nullptr);
		glClearTexImage(voxelNormal, 0, GL_RGBA, GL_FLOAT, nullptr);

		glm::mat4 projection = glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, 0.0f, 40.0f);
		glm::mat4 mvp_x = projection * glm::lookAt(glm::vec3(20, 0, 0), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
		glm::mat4 mvp_y = projection * glm::lookAt(glm::vec3(0, 20, 0), glm::vec3(0, 0, 0), glm::vec3(0, 0, -1));
		glm::mat4 mvp_z = projection * glm::lookAt(glm::vec3(0, 0, 20), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

		voxelProgram.bind();
		voxelProgram.setUniformMatrix4fv("mvp_x", mvp_x);
		voxelProgram.setUniformMatrix4fv("mvp_y", mvp_y);
		voxelProgram.setUniformMatrix4fv("mvp_z", mvp_z);

		voxelProgram.setUniform1i("axis_override", settings.axisOverride);

		voxelProgram.setUniform3fv("eye", camera.position);
		voxelProgram.setUniform3fv("lightPos", mainlight.position);
		voxelProgram.setUniform3fv("lightInt", mainlight.intensity);

		GLenum voxelFormat = useRGBA16f ? GL_RGBA16F : GL_R32UI;
		glBindImageTexture(0, voxelColor, 0, GL_TRUE, 0, GL_READ_WRITE, voxelFormat);
		glBindImageTexture(1, voxelNormal, 0, GL_TRUE, 0, GL_READ_WRITE, voxelFormat);

		scene->draw(voxelProgram.getHandle());

		glBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
		glBindImageTexture(1, 0, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
		voxelProgram.unbind();


		// Restore OpenGL state
		glViewport(0, 0, width, height);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		glDepthMask(GL_TRUE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (GLAD_GL_NV_conservative_raster && settings.conservativeRasterization) {
			glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
		}
		GL_DEBUG_POP()
	}
	voxelizeTimer.stop();

	// Generate shadowmap
	shadowmapTimer.start();
	const float lz_near = 0.0f, lz_far = 100.0f, l_boundary = 25.0f;
	glm::mat4 lp = glm::ortho(-l_boundary, l_boundary, -l_boundary, l_boundary, lz_near, lz_far);
	glm::mat4 lv = glm::lookAt(mainlight.position, mainlight.position + mainlight.direction, glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 ls = lp * lv;
	{
		GL_DEBUG_PUSH("Shadowmap")
		shadowmapFBO.bind();
		glViewport(0, 0, SHADOWMAP_WIDTH, SHADOWMAP_HEIGHT);
		glClear(GL_DEPTH_BUFFER_BIT);
		glDisable(GL_BLEND);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);

		glm::mat4 model;

		shadowmapProgram.bind();
		shadowmapProgram.setUniformMatrix4fv("projection", lp);
		shadowmapProgram.setUniformMatrix4fv("view", lv);
		shadowmapProgram.setUniformMatrix4fv("model", model);

		scene->draw(shadowmapProgram.getHandle());

		shadowmapProgram.unbind();
		shadowmapFBO.unbind();

		glCullFace(GL_BACK);
		GL_DEBUG_POP()
	}
	shadowmapTimer.stop();

	// Normalize voxelColor and voxelNormal textures (divides by alpha component)
	if (useRGBA16f) {
		static GLShaderProgram *p = nullptr;
		if (!p) {
			p = new GLShaderProgram({SHADER_DIR "normalizeVoxels.comp"});
			p->setObjectLabel("Normalize Voxels");
		}
		p->bind();
		glBindImageTexture(0, voxelColor, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
		glBindImageTexture(1, voxelNormal, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);

		glDispatchCompute((voxelDim + 4 - 1) / 4, (voxelDim + 4 - 1) / 4, (voxelDim + 4 - 1) / 4);

		glBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
		glBindImageTexture(1, 0, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
		p->unbind();
	}

	// Inject radiance into voxel grid
	radianceTimer.start();
	{
		GL_DEBUG_PUSH("Radiance Injection")

		glClearTexImage(voxelRadiance, 0, GL_RGBA, GL_FLOAT, nullptr);

		injectRadianceProgram.bind();

		glBindImageTexture(0, voxelColor, 0, GL_TRUE, 0, GL_READ_ONLY, useRGBA16f ? GL_RGBA16F : GL_RGBA8);
		glBindImageTexture(1, voxelNormal, 0, GL_TRUE, 0, GL_READ_ONLY, useRGBA16f ? GL_RGBA16F : GL_RGBA8);
		glBindImageTexture(2, voxelRadiance, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);

		GLuint shadowmap = shadowmapFBO.getTexture(0);
		glBindTextureUnit(1, shadowmap);
		injectRadianceProgram.setUniform1i("shadowmap", 1);

		glm::mat4 lsInverse = glm::inverse(ls);
		injectRadianceProgram.setUniformMatrix4fv("lsInverse", lsInverse);
		injectRadianceProgram.setUniform3fv("lightPos", mainlight.position);
		injectRadianceProgram.setUniform3fv("lightInt", mainlight.intensity);

		injectRadianceProgram.setUniform1i("voxelDim", voxelDim);

		// 2D workgroup should be the size of shadowmap, local_size = 16
		glDispatchCompute((SHADOWMAP_WIDTH + 16 - 1) / 16, (SHADOWMAP_HEIGHT + 16 - 1) / 16, 1);

		glBindTextureUnit(1, 0);
		glBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
		glBindImageTexture(1, 0, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
		glBindImageTexture(2, 0, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
		injectRadianceProgram.unbind();

		GL_DEBUG_POP()
	}
	radianceTimer.stop();

	mipmapTimer.start();
	{
		// glGenerateTextureMipmap(voxelColor);
		// glGenerateTextureMipmap(voxelNormal);
		// glGenerateTextureMipmap(voxelRadiance);

		mipmapProgram.bind();

		int dim = voxelDim;
		const int local_size = 8;
		for (int level = 0; level < voxelLevels; level++) {
			glBindImageTexture(0, voxelRadiance, level, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA8);
			glBindImageTexture(1, voxelRadiance, level + 1, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);

			GLuint num_groups = ((dim >> 1) + local_size - 1) / local_size;
			glDispatchCompute(num_groups, num_groups, num_groups);

			glBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA8);
			glBindImageTexture(1, 0, 1, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);

			dim >>= 1;
		}

		mipmapProgram.unbind();
	}
	mipmapTimer.stop();

	renderTimer.start();
	// Render scene
	{
		GL_DEBUG_PUSH("Render Scene")
		glm::mat4 projection = glm::perspective(camera.fov, (float)width / height, near, far);
		glm::mat4 view = camera.lookAt();
		glm::mat4 model;

		glViewport(0, 0, width, height);
		// glEnable(GL_FRAMEBUFFER_SRGB);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		glPolygonMode(GL_FRONT_AND_BACK, settings.drawWireframe ? GL_LINE : GL_FILL);

		program.bind();
		program.setUniformMatrix4fv("projection", projection);
		program.setUniformMatrix4fv("view", view);
		program.setUniformMatrix4fv("model", model);
		program.setUniform3fv("eye", camera.position);
		program.setUniform3fv("lightPos", mainlight.position);
		program.setUniform3fv("lightInt", mainlight.intensity);
		program.setUniformMatrix4fv("ls", ls);
		program.setUniform1i("texture0", 0);

		GLuint shadowmap = shadowmapFBO.getTexture(0);
		glBindTextureUnit(1, shadowmap);
		program.setUniform1i("shadowmap", 1);

		program.setUniform1i("voxelize", settings.drawVoxels);
		program.setUniform1i("normals", settings.drawNormals);
		program.setUniform1i("dominant_axis", settings.drawDominantAxis);
		program.setUniform1i("radiance", settings.drawRadiance);

		program.setUniform1i("enableShadows", settings.enableShadows);
		program.setUniform1i("enableNormalMap", settings.enableNormalMap);
		program.setUniform1i("enableIndirect", settings.enableIndirect);
		program.setUniform1i("enableDiffuse", settings.enableDiffuse);
		program.setUniform1i("enableSpecular", settings.enableSpecular);
		program.setUniform1f("ambientScale", settings.ambientScale);

		program.setUniform1i("voxelDim", voxelDim);

		glBindTextureUnit(2, voxelColor);
		program.setUniform1i("voxelColor", 2);
		program.setUniform1i("miplevel", settings.miplevel);
		program.setUniform1i("voxelDim", voxelDim);

		glBindTextureUnit(3, voxelNormal);
		program.setUniform1i("voxelNormal", 3);

		glBindTextureUnit(4, voxelRadiance);
		program.setUniform1i("voxelRadiance", 4);

		program.setUniform1i("vctSteps", settings.vctSteps);
		program.setUniform1f("vctBias", settings.vctBias);
		program.setUniform1f("vctConeAngle", settings.vctConeAngle);
		program.setUniform1f("vctConeInitialHeight", settings.vctConeInitialHeight);
		program.setUniform1f("vctLodOffset", settings.vctLodOffset);

		scene->draw(program.getHandle());

		glBindTextureUnit(1, 0);
		glBindTextureUnit(2, 0);
		glBindTextureUnit(3, 0);
		glBindTextureUnit(4, 0);
		program.unbind();

		// glDisable(GL_FRAMEBUFFER_SRGB);
		if (settings.drawWireframe) {
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}
		GL_DEBUG_POP()
	}
	renderTimer.stop();
	totalTimer.stop();

	voxelizeTimer.getQueryResult();
	shadowmapTimer.getQueryResult();
	radianceTimer.getQueryResult();
	mipmapTimer.getQueryResult();
	renderTimer.getQueryResult();
	totalTimer.getQueryResult();

	// hacky view of shadowmap
	if (settings.drawShadowmap) {
		view2DTexture(shadowmapFBO.getTexture(0));
	}

	// hacky raymarcher
	if (settings.raymarch) {
		viewRaymarched();
	}

	// Render overlay
	{
		GL_DEBUG_PUSH("Render Overlay")
		ui.render(dt);
		GL_DEBUG_POP()
	}
}

// Create a 3D texture
GLuint make3DTexture(GLsizei size, GLsizei levels, GLenum internalFormat, GLint minFilter, GLint magFilter) {
	GLuint handle;

	glGenTextures(1, &handle);
	glBindTexture(GL_TEXTURE_3D, handle);

	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, minFilter);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, magFilter);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	glTexStorage3D(GL_TEXTURE_3D, levels, internalFormat, size, size, size);
	glClearTexImage(handle, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

	if (levels > 1) {
		glGenerateMipmap(GL_TEXTURE_3D);
	}

	glBindTexture(GL_TEXTURE_3D, 0);

	return handle;
}

// Dirty function that renders a texture to a full screen quad.
void view2DTexture(GLuint texture) {
	static const GLchar *vert =
		"#version 330\n"
		"layout(location = 0) in vec3 pos;\n"
		"layout(location = 1) in vec2 tc;\n"
		"out vec2 fragTexcoord;\n"
		"void main() {\n"
		"gl_Position = vec4(pos, 1);\n"
		"fragTexcoord = tc;\n"
		"}\n";

	static const GLchar *frag =
		"#version 420\n"
		"in vec2 fragTexcoord;\n"
		"out vec4 color;\n"
		"layout(binding = 0) uniform sampler2D texture0;\n"
		"void main() {\n"
		"color = vec4(texture(texture0, fragTexcoord).rgb, 1);\n"
		"}\n";

	static GLuint program = 0;
	if (program == 0) {
		GLuint shaders[2];
		shaders[0] = GLHelper::createShaderFromString(GL_VERTEX_SHADER, vert);
		shaders[1] = GLHelper::createShaderFromString(GL_FRAGMENT_SHADER, frag);
		program = glCreateProgram();
		glAttachShader(program, shaders[0]);
		glAttachShader(program, shaders[1]);
		glLinkProgram(program);
		if (!GLHelper::checkShaderProgramStatus(program)) {
			LOG_ERROR("Quad debug shader compilation failed");
		}
	}

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(program);

	glBindTextureUnit(0, texture);
	GLQuad::draw();

	glUseProgram(0);
}

void Application::viewRaymarched() {
	static const GLchar *vert =
		"#version 330\n"
		"layout(location = 0) in vec3 pos;\n"
		"layout(location = 1) in vec2 tc;\n"
		"out vec2 fragTexcoord;\n"
		"void main() {\n"
		"gl_Position = vec4(pos, 1);\n"
		"fragTexcoord = tc;\n"
		"}\n";

	static GLuint program = 0;
	if (program == 0) {
		GLuint shaders[2];
		shaders[0] = GLHelper::createShaderFromString(GL_VERTEX_SHADER, vert);
		shaders[1] = GLHelper::createShaderFromFile(GL_FRAGMENT_SHADER, SHADER_DIR "raymarch.frag");
		program = glCreateProgram();
		glAttachShader(program, shaders[0]);
		glAttachShader(program, shaders[1]);
		glLinkProgram(program);
		if (!GLHelper::checkShaderProgramStatus(program)) {
			LOG_ERROR("Raymarch debug shader compilation failed");
		}
	}

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(program);

	glBindTextureUnit(0, voxelColor);
	glBindTextureUnit(1, voxelNormal);
	glBindTextureUnit(2, voxelRadiance);

	glm::vec3 cameraRight = glm::normalize(glm::cross(camera.front, camera.up)) * ((float)width / height);

	glUniform3fv(glGetUniformLocation(program, "eye"), 1, glm::value_ptr(camera.position));
	glUniform3fv(glGetUniformLocation(program, "viewForward"), 1, glm::value_ptr(camera.front));
	glUniform3fv(glGetUniformLocation(program, "viewRight"), 1, glm::value_ptr(cameraRight));
	glUniform3fv(glGetUniformLocation(program, "viewUp"), 1, glm::value_ptr(glm::normalize(glm::cross(cameraRight, camera.front))));
	glUniform1i(glGetUniformLocation(program, "width"), width);
	glUniform1i(glGetUniformLocation(program, "height"), height);
	glUniform1f(glGetUniformLocation(program, "near"), near);
	glUniform1f(glGetUniformLocation(program, "far"), far);
	glUniform1i(glGetUniformLocation(program, "voxelDim"), voxelDim);
	glUniform1i(glGetUniformLocation(program, "lod"), settings.miplevel);
	

	GLQuad::draw();

	glUseProgram(0);
}
