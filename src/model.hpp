﻿#pragma once

//
// Cinder 0.9.0 でのモデル表示
//

#include <cinder/AxisAlignedBox.h>

#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>     // Post processing flags


// ライブラリ読み込み指定
#if defined (_MSC_VER)
#if defined (_DEBUG)
#pragma comment(lib, "assimp-mtd")
#else
#pragma comment(lib, "assimp-mt")
#endif
#endif


// ウェイト値が小さすぎる場合への対処
#define WEIGHT_WORKAROUND
// フルパス指定
#define USE_FULL_PATH


#include <map>
#include <set>
#include <limits>

#include "common.hpp"
#include "mesh.hpp"
#include "material.hpp"
#include "texture.hpp"
#include "node.hpp"
#include "animation.hpp"


struct Model {
  std::vector<Material> material;

  // マテリアルからのテクスチャ参照は名前引き
  std::map<std::string, ci::gl::Texture2dRef> textures;

  // 親子関係にあるノード
  std::shared_ptr<Node> node;

  // 名前からノードを探す用(アニメーションで使う)
  std::map<std::string, std::shared_ptr<Node> > node_index;

  // 親子関係を解除した状態(全ノードの行列を更新する時に使う)
  std::vector<std::shared_ptr<Node> > node_list;

  bool has_anim;
  std::vector<Anim> animation;

  ci::AxisAlignedBox aabb;

  // FIXME:ここで持つべきではない
  


  
#if defined (USE_FULL_PATH)
  // 読み込みディレクトリ
  std::string directory;
#endif
};


#if defined (WEIGHT_WORKAROUND)

// メッシュのウェイトを正規化
//   ウェイト編集時にウェイトが極端に小さい頂点が発生しうる
//   その場合に見た目におかしくなってしまうのをいい感じに直す
void normalizeMeshWeight(Model& model) {
  for (const auto& node : model.node_list) {
    for (auto& mesh : node->mesh) {
      if (!mesh.has_bone) continue;

      // 各頂点へのウェイト書き込みを記録
      std::set<u_int> weight_index;
      std::multimap<u_int, Weight*> weight_values;

      for (auto& bone : mesh.bones) {
        for (auto& weight : bone.weights) {
          weight_index.insert(weight.vertex_id);
          weight_values.emplace(weight.vertex_id, &weight);
        }
      }

      // 正規化
      for (const auto i : weight_index) {
        const auto p = weight_values.equal_range(i);
        float weight = 0.0f;
        for (auto it = p.first; it != p.second; ++it) {
          weight += it->second->value;
        }
        assert(weight > 0.0f);

        {
          // ci::app::console() << "Weight min: " << weight << std::endl;
          float n = 1.0f / weight;
          for (auto it = p.first; it != p.second; ++it) {
            it->second->value *= n;
          }
        }
      }
    }
  }
}

#endif


// モデルの全頂点数とポリゴン数を数える
std::pair<size_t, size_t> getMeshInfo(const Model& model) {
  size_t vertex_num   = 0;
  size_t triangle_num = 0;

  for (const auto& node : model.node_list) {
    for (const auto& mesh : node->mesh) {
      vertex_num += mesh.body.getNumVertices();
      triangle_num += mesh.body.getNumIndices() / 3;
    }
  }

  return std::make_pair(vertex_num, triangle_num);
}


// 階層アニメーション用の行列を計算
void updateNodeMatrix(Model& model, const double time, const Anim& animation) {
  for (const auto& body : animation.body) {
    // 階層アニメーションを取り出して行列を生成
    ci::mat4 m;
    m = ci::translate(m, getLerpValue(time, body.translate));
    ci::mat4 r = glm::toMat4(getLerpValue(time, body.rotation));
    m = m * r;
    m = ci::scale(m, getLerpValue(time, body.scaling));

    // ノードの行列を書き換える
    auto node = model.node_index.at(body.node_name);
    node->matrix = m;
  }
}

void updateMesh(Model& model) {
  for (const auto& node : model.node_list) {
    for (auto& mesh : node->mesh) {
      if (!mesh.has_bone) continue;

      // 座標変換に必要な行列を用意
      std::vector<ci::mat4> bone_matrix;
      bone_matrix.reserve(mesh.bones.size());
      for (auto& bone : mesh.bones) {
        auto local_node = model.node_index.at(bone.name);
        bone_matrix.push_back(node->invert_matrix * local_node->global_matrix * bone.offset);
      }

      // 変換結果を書き出す頂点配列
      auto* body_vtx    = mesh.body.getPositions<3>();
      auto& body_normal = mesh.body.getNormals();

      size_t num = mesh.body.getNumVertices();
      
      std::fill(body_vtx, body_vtx + num, ci::vec3(0.0f));
      if (mesh.body.hasNormals()) {
        std::fill(body_normal.begin(), body_normal.end(), ci::vec3(0.0f));
      }

      // オリジナルの頂点データ
      const auto* orig_vtx    = mesh.orig.getPositions<3>();
      const auto& orig_normal = mesh.orig.getNormals();

      // 全頂点の座標を再計算
      for (u_int i = 0; i < mesh.bones.size(); ++i) {
        const auto& bone = mesh.bones[i];
        const auto& m    = bone_matrix[i];

        for (const auto& weight : bone.weights) {
          body_vtx[weight.vertex_id] += ci::vec3(m * ci::vec4(orig_vtx[weight.vertex_id], 1.0f)) * weight.value;
        }

        if (mesh.body.hasNormals()) {
          for (const auto& weight : bone.weights) {
            body_normal[weight.vertex_id] += ci::vec3(m * ci::vec4(orig_normal[weight.vertex_id], 0.0f)) * weight.value;
          }
        }
      }
    }
  }
}

// アニメーションによるノード更新
void updateModel(Model& model, const double time, const size_t index) {
  if (!model.has_anim) return;

  // 最大時間でループさせている
  double current_time = std::fmod(time, model.animation[index].duration);

  // アニメーションで全ノードの行列を更新
  updateNodeMatrix(model, current_time, model.animation[index]);

  // ノードの行列を再計算
  updateNodeDerivedMatrix(model.node, ci::mat4());

  // メッシュアニメーションを適用
  updateMesh(model);
}

// 全頂点を元に戻す
void resetMesh(Model& model) {
  for (const auto& node : model.node_list) {
    for (auto& mesh : node->mesh) {
      if (!mesh.has_bone) continue;

      mesh.body = mesh.orig;
    }
  }
}

// ノードの行列をリセット
void resetModelNodes(Model& model) {
  for (const auto& node : model.node_list) {
    node->matrix = node->matrix_orig;
  }

  resetMesh(model);
}

// ざっくりAABBを求める
//   アニメーションで変化するのは考慮しない
ci::AxisAlignedBox calcAABB(Model& model) {
  // ノードの行列を更新
  updateNodeDerivedMatrix(model.node, ci::mat4());

  // スケルタルアニメーションを考慮
  updateModel(model, 0.0, 0);

  // 最小値を格納する値にはその型の最大値を
  // 最大値を格納する値にはその型の最小値を
  // それぞれ代入しておく
  float max_value = std::numeric_limits<float>::max();
  ci::vec3 min_vtx{ max_value, max_value, max_value };

  float min_value = std::numeric_limits<float>::min();
  ci::vec3 max_vtx{ min_value, min_value, min_value };

  // 全頂点を調べてAABBの頂点座標を割り出す
  for (const auto& node : model.node_list) {
    for (const auto& mesh : node->mesh) {
      const auto* verticies = mesh.body.getPositions<3>();
      size_t num = mesh.body.getNumVertices();
      for (size_t i = 0; i < num; ++i) {
        // ノードの行列でアフィン変換
        ci::vec3 tv(node->global_matrix * ci::vec4(verticies[i], 1.0f));

        min_vtx.x = std::min(tv.x, min_vtx.x);
        min_vtx.y = std::min(tv.y, min_vtx.y);
        min_vtx.z = std::min(tv.z, min_vtx.z);

        max_vtx.x = std::max(tv.x, max_vtx.x);
        max_vtx.y = std::max(tv.y, max_vtx.y);
        max_vtx.z = std::max(tv.z, max_vtx.z);
      }
    }
  }

  return ci::AxisAlignedBox(min_vtx, max_vtx);
}


// モデル読み込み
Model loadModel(const std::string& path) {
  Assimp::Importer importer;

  const aiScene* scene = importer.ReadFile(path,
                                           aiProcess_Triangulate
                                           | aiProcess_JoinIdenticalVertices
                                           | aiProcess_OptimizeMeshes
                                           | aiProcess_RemoveRedundantMaterials);

  assert(scene);
  
  Model model;

#if defined (USE_FULL_PATH)
  // ファイルの親ディレクトリを取得
  ci::fs::path full_path{ path };
  model.directory = full_path.parent_path().string();
#endif

  if (scene->HasMaterials()) {
    u_int num = scene->mNumMaterials;
    ci::app::console() << "Materials:" << num << std::endl;

    aiMaterial** mat = scene->mMaterials;
    for (u_int i = 0; i < num; ++i) {
      model.material.push_back(createMaterial(mat[i]));

      // テクスチャ読み込み
      const auto& m = model.material.back();
      if (!m.has_texture) continue;

#if defined (USE_FULL_PATH)
      std::string path = model.directory + "/" + PATH_WORKAROUND(m.texture_name);
      auto texture = loadTexrture(path);
#else
      auto texture = loadTexrture(PATH_WORKAROUND(m.texture_name));
#endif

      model.textures.insert(std::make_pair(m.texture_name, texture));
    }
  }

  model.node = createNode(scene->mRootNode, scene->mMeshes);

  // ノードを名前から探せるようにする
  createNodeInfo(model.node,
                 model.node_index,
                 model.node_list);

  model.has_anim = scene->HasAnimations();
  if (model.has_anim) {
    ci::app::console() << "Animations:" << scene->mNumAnimations << std::endl;

    aiAnimation** anim = scene->mAnimations;
    for (u_int i = 0; i < scene->mNumAnimations; ++i) {
      model.animation.push_back(createAnimation(anim[i]));
    }
  }

#if defined (WEIGHT_WORKAROUND)
  normalizeMeshWeight(model);
#endif

  model.aabb = calcAABB(model);

  auto info = getMeshInfo(model);

  ci::app::console() << "Total vertex num:" << info.first << " triangle num:" << info.second << std::endl;

  return model;
}


// モデル描画
// TIPS:全ノード最終的な行列が計算されているので、再帰で描画する必要は無い
void drawModel(const Model& model,
               const ci::gl::GlslProgRef& color, const ci::gl::GlslProgRef& texture) {
  for (const auto& node : model.node_list) {
    if (node->mesh.empty()) continue;
    ci::gl::pushModelView();
    ci::gl::multModelMatrix(node->global_matrix);

    for (const auto& mesh : node->mesh) {
      const auto& material = model.material[mesh.material_index];

      if (material.has_texture) {
        model.textures.at(material.texture_name)->bind();
        texture->bind();
      }
      else {
        color->bind();
      }

      ci::gl::draw(mesh.body);

      if (material.has_texture) {
        model.textures.at(material.texture_name)->unbind();
      }
      
    }
    ci::gl::popModelView();
  }
}

// 描画順を逆にする
void reverseModelNode(Model& model) {
  for (const auto& node : model.node_list) {
    std::reverse(std::begin(node->mesh), std::end(node->mesh));
  }
  
  std::reverse(std::begin(model.node_list), std::end(model.node_list));
}
