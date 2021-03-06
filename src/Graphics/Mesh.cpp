#include "Mesh.h"

#include <Graphics/opengl.h>
#include <Graphics/GLHelper.h>
#include <glm/glm.hpp>
#include <glm/gtx/string_cast.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <chrono>
#include <limits>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <stb_image.h>

#include <unordered_map>
#include <common.h>

using namespace std;
using namespace tinyobj;

static const char *DEFAULT_TEXTURE = "default_texture.png";

void convertPathFromWindows(std::string &str) {
#ifndef _WIN32
    replace(begin(str), end(str), '\\', '/');
#endif
}

Mesh::Mesh(const std::string &meshname) {
    loadMesh(meshname);
}

// modified from https://github.com/syoyo/tinyobjloader/blob/master/examples/viewer/viewer.cc
void Mesh::loadMesh(const std::string &meshname) {
    string err;
    string basedir = meshname.substr(0, meshname.find_last_of('/') + 1);
    bool status = LoadObj(&attrib, &shapes, &materials, &err, meshname.c_str(), basedir.c_str());

    if (!err.empty()) {
        LOG_ERROR(err);
    }

    if (!status) {
        LOG_ERROR("Failed to load mesh: ", meshname);
    }
    else {
        auto start = chrono::high_resolution_clock::now();

        // Load materials and store name->id map
        for (material_t &mp : materials) {
            // Material m;
            // for (int i = 0; i < 3; i++) {
            //     m.ambient[i] = mp.ambient[i];
            //     m.diffuse[i] = mp.diffuse[i];
            //     m.specular[i] = mp.specular[i];
            // }
            // m.shininess = mp.shininess;
            // m.hasAmbientMap = !mp.diffuse_texname.empty();
            // m.hasDiffuseMap = !mp.ambient_texname.empty();
            // m.hasSpecularMap = !mp._texname.empty();
            // m.hasAlphaMap = !mp.diffuse_texname.empty();
            // m.hasNormalMap = !mp.diffuse_texname.empty();

            if (!mp.diffuse_texname.empty() && textures.find(mp.diffuse_texname) == textures.end()) {
                string texture_name = mp.diffuse_texname;
                convertPathFromWindows(texture_name);

                texture_name = basedir + texture_name;
                GLuint texture_id = GLHelper::createTextureFromImage(texture_name);
                
                textures.insert(make_pair(mp.diffuse_texname, texture_id));
                LOG_INFO("loaded diffuse ", mp.diffuse_texname);
            }

            if (!mp.bump_texname.empty() && textures.find(mp.bump_texname) == textures.end()) {
                string texture_name = mp.bump_texname;
                convertPathFromWindows(texture_name);

                texture_name = basedir + texture_name;
                int width, height, channels;
                unsigned char *image = stbi_load(texture_name.c_str(), &width, &height, &channels, STBI_default);
                if (image == nullptr) {
                    LOG_ERROR("TEXTURE::LOAD_FAILED::", texture_name);
                    continue;
                }
                else if (channels == 3 || channels == 4) {
                    GLuint texture_id;
                    glCreateTextures(GL_TEXTURE_2D, 1, &texture_id);
                    
                    glTextureParameteri(texture_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
                    glTextureParameteri(texture_id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTextureParameteri(texture_id, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTextureParameteri(texture_id, GL_TEXTURE_WRAP_T, GL_REPEAT);

                    if (GLAD_GL_EXT_texture_filter_anisotropic) {
                        float maxAnisotropy = 1.0f;
                        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAnisotropy);
                        glTextureParameterf(texture_id, GL_TEXTURE_MAX_ANISOTROPY, maxAnisotropy);
                    }

                    GLint levels = (GLint)std::log2(std::fmax(width, height)) + 1;
                    if (channels == 3) {
                        glTextureStorage2D(texture_id, levels, GL_RGB8, width, height);
                        glTextureSubImage2D(texture_id, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, image);
                    }
                    else if (channels == 4) {
                        glTextureStorage2D(texture_id, levels, GL_RGBA8, width, height);
                        glTextureSubImage2D(texture_id, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image); 
                    }

                    glGenerateTextureMipmap(texture_id);

                    textures.insert(make_pair(mp.bump_texname, texture_id));
                    LOG_INFO("loaded normal map ", mp.bump_texname);
                }
                else {
                    LOG_WARN("Bump map ", texture_name, " not supported, convert it to normal map");
                }
                stbi_image_free(image);
            }
        }

        // Default material
        {
            material_t default_material;
            default_material.name = "default";
            default_material.diffuse_texname = DEFAULT_TEXTURE;
            default_material.shininess = 1.0f;
                        
            materials.push_back(default_material);

            GLuint texture_id = GLHelper::createTextureFromImage(string(RESOURCE_DIR) + default_material.diffuse_texname);
            textures.insert(make_pair(default_material.diffuse_texname, texture_id));
        }

        drawables.resize(materials.size());
        for (size_t i = 0; i < drawables.size(); i++) {
            drawables[i].material_id = i;
        }

        min = glm::vec3(numeric_limits<float>::max());
        max = glm::vec3(numeric_limits<float>::min());

        unordered_map<string, size_t> vertexMap;
        vertexMap.clear();
        vertices.clear();
        for (const auto &shape : shapes) {
            size_t index_offset = 0;

            // Loop through each face
            for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
                // Number of vertices per face (should always be 3 since triangulate=true)
                unsigned char fv = shape.mesh.num_face_vertices[f];
                assert(fv == 3);

                // Per face material_id
                int material_id = shape.mesh.material_ids[f];
                auto &d = drawables[material_id == -1 ? drawables.size() - 1 : material_id];

                // Store pointers to each vertex so we can process per-face attributes later
                vector<size_t> tri (fv, 0);

                // Loop through each vertex of the current face
                for (size_t v = 0; v < fv; v++) {
                    index_t index = shape.mesh.indices[index_offset + v];
                    // assert(index.vertex_index == index.normal_index && index.normal_index == index.texcoord_index);
                    string index_str =
                        to_string(index.vertex_index) + string("_")
                        + to_string(index.normal_index) + string("_")
                        + to_string(index.texcoord_index);
                    size_t vertIndex = 0;
                    if (vertexMap.count(index_str) == 0) {
                        // new vertex, add to map and load positions, normals, texcoords
                        vertIndex = vertices.size();
                        vertexMap.insert(make_pair(index_str, vertIndex));
                        vertices.push_back(Vertex{});
                        tri[v] = vertIndex;
                        
                        vertices[tri[v]].position[0] = attrib.vertices[3 * index.vertex_index];
                        vertices[tri[v]].position[1] = attrib.vertices[3 * index.vertex_index + 1];
                        vertices[tri[v]].position[2] = attrib.vertices[3 * index.vertex_index + 2];
                        if (index.normal_index >= 0) {
                            vertices[tri[v]].normal[0] = attrib.normals[3 * index.normal_index];
                            vertices[tri[v]].normal[1] = attrib.normals[3 * index.normal_index + 1];
                            vertices[tri[v]].normal[2] = attrib.normals[3 * index.normal_index + 2];
                        }
                        if (index.texcoord_index >= 0) {
                            vertices[tri[v]].texcoord[0] = attrib.texcoords[2 * index.texcoord_index];
                            vertices[tri[v]].texcoord[1] = attrib.texcoords[2 * index.texcoord_index + 1];
                        }
                    }
                    else {
                        // existing vertex 
                        vertIndex = vertexMap.at(index_str);
                        tri[v] = vertIndex;
                    }
                    d.indices.push_back(vertIndex);
                }

                // https://learnopengl.com/Advanced-Lighting/Normal-Mapping
                glm::vec3 edge1 = vertices[tri[1]].position - vertices[tri[0]].position;
                glm::vec3 edge2 = vertices[tri[2]].position - vertices[tri[0]].position;
                glm::vec2 deltaUV1 = vertices[tri[1]].texcoord - vertices[tri[0]].texcoord;
                glm::vec2 deltaUV2 = vertices[tri[2]].texcoord - vertices[tri[0]].texcoord;
                float invDeterminant = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
                glm::vec3 tangent, bitangent;
                tangent.x = invDeterminant * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
                tangent.y = invDeterminant * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
                tangent.z = invDeterminant * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
                bitangent.x = invDeterminant * (deltaUV2.x * edge1.x - deltaUV1.x * edge2.x);
                bitangent.y = invDeterminant * (deltaUV2.x * edge1.y - deltaUV1.x * edge2.y);
                bitangent.z = invDeterminant * (deltaUV2.x * edge1.z - deltaUV1.x * edge2.z);
                for (size_t v = 0; v < 3; v++) {
                    vertices[tri[v]].tangent += tangent;
                    vertices[tri[v]].bitangent += bitangent;
                }

                index_offset += fv;
            }
        }

        for (Vertex &v : vertices) {
            v.tangent = glm::normalize(v.tangent);
            v.bitangent = glm::normalize(v.bitangent);

            min = glm::min(min, v.position);
            max = glm::max(max, v.position);
        }

        glm::vec3 extents = max - min;
        radius = glm::max(glm::max(extents.x, extents.y), extents.z) / 2.0f;

        for (Drawable &d : drawables) {
            glGenBuffers(1, &d.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, d.indices.size() * sizeof(GLuint), d.indices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }

        GLuint buf = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &buf);

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buf);

        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid *)offsetof(Vertex, position));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid *)offsetof(Vertex, normal));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid *)offsetof(Vertex, texcoord));
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid *)offsetof(Vertex, tangent));
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid *)offsetof(Vertex, bitangent));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        glEnableVertexAttribArray(4);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        auto end = chrono::high_resolution_clock::now();
        chrono::duration<double> diff = end - start;
        LOG_INFO(
            "\n\tLoaded mesh ", meshname, " in ", diff.count(), " seconds",
            "\n\t# of vertices  = ", (int)(attrib.vertices.size()) / 3,
            "\n\t# of normals   = ", (int)(attrib.normals.size()) / 3,
            "\n\t# of texcoords = ", (int)(attrib.texcoords.size()) / 2,
            "\n\t# of materials = ", (int)materials.size(),
            "\n\t# of shapes    = ", (int)shapes.size(),
            "\n\tmin = ", glm::to_string(min),
            "\n\tmax = ", glm::to_string(max),
            "\n\tradius = ", radius
        );
    }
}

void Mesh::draw(GLuint program) const {
    GLint enableNormalMapLocation = glGetUniformLocation(program, "enableNormalMap");
    GLint enableNormalMap = 0;
    if  (enableNormalMapLocation >= 0)
        glGetUniformiv(program, enableNormalMapLocation, &enableNormalMap);

    glBindVertexArray(vao);
    for (const auto &d : drawables) {
        const auto &m = materials[d.material_id];

        GLuint default_texture = textures.at(DEFAULT_TEXTURE);
        bool hasDiffuseMap = textures.count(m.diffuse_texname) == 0;
        bool hasNormalMap = textures.count(m.bump_texname) == 0;

        GLuint diffuse_texture = hasDiffuseMap ? default_texture : textures.at(m.diffuse_texname);
        GLuint bump_texture;
        if (hasNormalMap) {
            bump_texture = 0;
            glUniform1i(enableNormalMapLocation, false);
        }
        else {
            bump_texture = textures.at(m.bump_texname);
            glUniform1i(enableNormalMapLocation, enableNormalMap);
        }

        glBindTextureUnit(0, diffuse_texture);
        glBindTextureUnit(5, bump_texture);

        glUniform3fv(glGetUniformLocation(program, "material.ambient"), 3, m.ambient);
        glUniform3fv(glGetUniformLocation(program, "material.diffuse"), 3, m.diffuse);
        glUniform3fv(glGetUniformLocation(program, "material.specular"), 3, m.specular);
        glUniform1f(glGetUniformLocation(program, "material.shininess"), m.shininess);
        glUniform1i(glGetUniformLocation(program, "material.hasAmbientMap"), false);
        glUniform1i(glGetUniformLocation(program, "material.hasDiffuseMap"), hasDiffuseMap);
        glUniform1i(glGetUniformLocation(program, "material.hasSpecularMap"), false);
        glUniform1i(glGetUniformLocation(program, "material.hasAlphaMap"), false);
        glUniform1i(glGetUniformLocation(program, "material.hasNormalMap"), hasNormalMap);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ebo);
        glDrawElements(GL_TRIANGLES, d.indices.size(), GL_UNSIGNED_INT, 0);
    }

    if  (enableNormalMapLocation >= 0)
        glUniform1i(enableNormalMapLocation, enableNormalMap);

    glBindTextureUnit(0, 0);
    glBindTextureUnit(5, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
