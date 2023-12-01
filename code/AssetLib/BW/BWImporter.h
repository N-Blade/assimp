#pragma once
#ifndef AI_BWIMPORTER_H_INC
#define AI_BWIMPORTER_H_INC

#include <assimp/types.h>
#include <assimp/mesh.h>
#include <assimp/material.h>
#include <assimp/BaseImporter.h>

#include <memory>
#include <vector>

struct aiNodeAnim;
struct aiNode;
struct aiAnimation;

namespace Assimp{

class BWImporter : public BaseImporter{
public:
    BWImporter();
    ~BWImporter() override;
    bool CanRead(const std::string& pFile, IOSystem* pIOHandler, bool checkSig) const override;

protected:
    const aiImporterDesc* GetInfo () const override;
    void InternReadFile(const std::string& pFile, aiScene* pScene, IOSystem* pIOHandler) override;

private:

    int ReadByte();
    int ReadInt();
    float ReadFloat();
    aiVector2D ReadVec2();
    aiVector3D ReadVec3();
    aiQuaternion ReadQuat();
    std::string ReadString();
    std::string ReadChunk();
    void ExitChunk();
    size_t ChunkSize();

    struct Vertex{
        aiVector3D vertex;
        aiVector3D normal;
        aiVector3D texcoords;
        unsigned char bones[4];
        float weights[4];
    };

    AI_WONT_RETURN void Oops() AI_WONT_RETURN_SUFFIX;
    AI_WONT_RETURN void Fail(const std::string& str) AI_WONT_RETURN_SUFFIX;

    void ReadTEXS();
    void ReadBRUS();

    void ReadVRTS();
    void ReadTRIS( int v0 );
    void ReadMESH();
    void ReadBONE( int id );
    void ReadKEYS( aiNodeAnim *nodeAnim );
    void ReadANIM();

    aiNode *ReadNODE( aiNode *parent );

    void ReadBB3D( aiScene *scene );

    size_t _pos;
    std::vector<unsigned char> _buf;
    std::vector<size_t> _stack;

    std::vector<std::string> _textures;
    std::vector<std::unique_ptr<aiMaterial> > _materials;

    int _vflags,_tcsets,_tcsize;
    std::vector<Vertex> _vertices;

    std::vector<aiNode*> _nodes;
    std::vector<std::unique_ptr<aiMesh> > _meshes;
    std::vector<std::unique_ptr<aiNodeAnim> > _nodeAnims;
    std::vector<std::unique_ptr<aiAnimation> > _animations;
};

}

#endif
