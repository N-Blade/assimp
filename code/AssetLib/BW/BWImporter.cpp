#ifndef ASSIMP_BUILD_NO_BW_IMPORTER

// internal headers
#include "AssetLib/BW/BWImporter.h"
#include "PostProcessing/ConvertToLHProcess.h"
#include "PostProcessing/TextureTransform.h"

#include <assimp/StringUtils.h>
#include <assimp/anim.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/XmlParser.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>

#define DEBUG_BW

std::string ai_trim(const char* s) {
    std::string value(s);
    return ::ai_trim(value);
}

std::string ai_to_string(std::set<std::string>& value) {
    std::ostringstream os;
    os << "{";
    size_t size = value.size();
    for (auto const& v: value) {
        os << v;
        if (--size) {
            os << ", ";
        }
    }
    os << "}";

    return os.str();
}

template <class T>
T *to_array(const std::vector<T> &v) {
    if (v.empty()) {
        return nullptr;
    }
    T *p = new T[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        p[i] = v[i];
    }
    return p;
}

template <class T>
T **unique_to_array(std::vector<std::unique_ptr<T>> &v) {
    if (v.empty()) {
        return nullptr;
    }
    T **p = new T *[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        p[i] = v[i].release();
    }
    return p;
}


namespace Assimp {
using namespace std;


static constexpr aiImporterDesc desc = {
    "BW Importer",
    "",
    "",
    "http://www.n-blade.ru",
    aiImporterFlags_SupportBinaryFlavour,
    0,
    0,
    0,
    0,
    "model visual" // primitives // visual
};

#ifdef _MSC_VER
#pragma warning(disable : 4018)
#endif

// #define DEBUG_B3D

template <typename T>
void DeleteAllBarePointers(std::vector<T> &x) {
    for (auto p : x) {
        delete p;
    }
}

BWImporter::BWImporter() = default;

BWImporter::~BWImporter() = default;

// ------------------------------------------------------------------------------------------------
bool BWImporter::CanRead(const std::string &pFile, IOSystem * /*pIOHandler*/, bool /*checkSig*/) const {
    std::string ext = BaseImporter::GetExtension(pFile);
    return ext == "model";
}

// ------------------------------------------------------------------------------------------------
// Loader meta information
const aiImporterDesc *BWImporter::GetInfo() const {
    return &desc;
}

struct VertexHeader {
    char vertexFormat_[64];
    uint32_t nVertices_;
};

struct Vector3 {
    float x;
    float y;
    float z;

    inline void setZero() {
        x = y = z = 0;
    }
};

struct Vector2 {
    float u;
    float v;
};

inline Vector3 unpackNormal(uint32_t packed) {
    int32_t z = int32_t(packed) >> 22;
    int32_t y = int32_t(packed << 10) >> 21;
    int32_t x = int32_t(packed << 21) >> 21;

    return Vector3{ float(x) / 1023.f, float(y) / 1023.f, float(z) / 511.f };
}


/**
 * Position, Normal, UV
 */
struct VertexXYZNUV	{
    Vector3 pos_;
    Vector3 normal_;
    Vector2 uv_;
};

/**
 * Position, Normal, UV, Packed Tangent, Packed Binormal
 */
struct VertexXYZNUVTB {
    Vector3 pos_;
    uint32_t normal_;
    Vector2 uv_;
    uint32_t tangent_;
    uint32_t binormal_;
};


/**
 * Position, Normal, UV, Tangent, Binormal
 */
struct VertexXYZNUVTBPC {
    Vector3		pos_;
    Vector3		normal_;
    Vector2		uv_;
    Vector3		tangent_;
    Vector3		binormal_;
    
    VertexXYZNUVTBPC& operator =(const VertexXYZNUVTB& in) {
        pos_ = in.pos_;
        normal_ = unpackNormal( in.normal_ );
        uv_ = in.uv_;
        tangent_ = unpackNormal( in.tangent_ );
        binormal_ = unpackNormal( in.binormal_ );
        return *this;
    }
    VertexXYZNUVTBPC(const VertexXYZNUVTB& in) {
        *this = in;
    }
    VertexXYZNUVTBPC& operator =(const VertexXYZNUV& in) {
        pos_ = in.pos_;
        normal_ = in.normal_;
        uv_ = in.uv_;
        tangent_.setZero();
        binormal_.setZero();
        return *this;
    }
    VertexXYZNUVTBPC(const VertexXYZNUV& in) {
        *this = in;
    }
    VertexXYZNUVTBPC() = default;
};

void loadVertices(const UByteBuffer& pVerticesSection, std::vector<VertexXYZNUVTBPC>& result) {
    // Get the vertex header
    const VertexHeader* pVH = reinterpret_cast<const VertexHeader*>(&pVerticesSection[0]);
    std::string vertexFormat = pVH->vertexFormat_;
    if (vertexFormat == "xyznuv") {
        const VertexXYZNUV* pVerts = reinterpret_cast<const VertexXYZNUV*>( pVH + 1 );
        result.assign( pVerts, pVerts + pVH->nVertices_ );
    }
    else if (vertexFormat == "xyznuvtb") {
        const VertexXYZNUVTB* pVerts = reinterpret_cast<const VertexXYZNUVTB*>( pVH + 1 );
        result.assign( pVerts, pVerts + pVH->nVertices_ );
    } else {
        throw DeadlyImportError("Unknown vertex format: ", vertexFormat, ".");
    }
}


struct IndexHeader {
    char indexFormat_[64];
    uint32_t nIndices_;
    uint32_t nTriangleGroups_;
};

// struct PrimitiveGroupc {
//     uint32_t groupIndex_;
//     EffectMaterialPtr material_;
// };

struct PrimitiveGroup {
    int32_t startIndex_;
    int32_t nPrimitives_;
    int32_t startVertex_;
    int32_t nVertices_;
};

enum D3DFORMAT {
    D3DFMT_INDEX16 = 101,
    D3DFMT_INDEX32 = 102,
};

class IndicesReference {
protected:
    void* indices_;
    uint32_t size_;
    D3DFORMAT format_;
public:
    IndicesReference() : indices_(nullptr), size_(0), format_(D3DFMT_INDEX16) {}
    virtual ~IndicesReference() {}
    virtual void assign(void* buffer, uint32_t numOfIndices, D3DFORMAT format) {
        indices_ = buffer;
        size_ = numOfIndices;
        format_ = format;
    }

    uint32_t operator[](int32_t index) const {
        return get(index);
    }
    void set(int32_t index, uint32_t value) {
        if (format_ == D3DFMT_INDEX16) {
            ((uint16_t*)indices_)[index] = (uint16_t)value;
        } else {
            ((uint32_t*)indices_)[index] = value;
        }
    }
    uint32_t get(int32_t index) const {
        if (index < 0 || static_cast<uint32_t>(index) >= size_) {
            throw DeadlyImportError("BW Importer - error in B3D file data: Bad triangle index ", index);
        }

        if (format_ == D3DFMT_INDEX16) {
            return ((uint16_t*)indices_)[index];
        } else {
            return ((uint32_t*)indices_)[index];
        }
    }
};

class IndicesHolder : public IndicesReference {
    IndicesHolder(const IndicesHolder&) = delete;
    void operator=(const IndicesHolder&) = delete;
public:
    IndicesHolder(D3DFORMAT format = D3DFMT_INDEX16, uint32_t entryNum = 0) {
        format_ = format;
        size_ = entryNum;
        if (size_) {
            indices_ = new unsigned char[ size() * entrySize() ];
        } else {
            indices_ = nullptr;
        }
    }
    ~IndicesHolder() {
        if (format_ == D3DFMT_INDEX16) {
            delete[] ((uint16_t *)indices_);
        } else {
            delete[] ((uint32_t *)indices_);
        }
    }

    int entrySize() const {
        return format_ == D3DFMT_INDEX16 ? 2 : 4;
    }
    uint32_t size() const {
        return size_;
    }
    void setSize(uint32_t entryNum, D3DFORMAT format) {
        if (format_ == D3DFMT_INDEX16) {
            delete[] ((uint16_t *)indices_);
        } else {
            delete[] ((uint32_t *)indices_);
        }

        indices_ = nullptr;
        format_ = format;
        if(entryNum != 0) {
            size_ = entryNum;
            indices_ = new unsigned char[ size() * entrySize() ];
        }
    }
    virtual void assign(const void* buffer, uint32_t entryNum, D3DFORMAT format) {
        // exception unsafe
        if (format_ == D3DFMT_INDEX16) {
            delete[] ((uint16_t *)indices_);
        } else {
            delete[] ((uint32_t *)indices_);
        }

        size_ = entryNum;
        format_ = format;
        indices_ = new unsigned char[ size() * entrySize() ];
        memcpy( indices_, buffer, size() * entrySize() );
    }
};


void loadIndices(const UByteBuffer& pIndicesSection, IndicesHolder& sourceIndices, std::vector<PrimitiveGroup>& sourcePrimitiveGroups) {
    // Get the index header
    const IndexHeader* pIH =  reinterpret_cast<const IndexHeader*>(&pIndicesSection[0]);

    // Only supports index lists, no strips.
    const std::string indexFormat = pIH->indexFormat_;
    // Make a local copy of the indices
    if (indexFormat == "list") {
        sourceIndices.assign((pIH + 1), pIH->nIndices_, D3DFMT_INDEX16);
    } else if (indexFormat == "list32") {
        sourceIndices.assign((pIH + 1), pIH->nIndices_, D3DFMT_INDEX32);
    } else {
        throw DeadlyImportError("Unknow indices format: ", indexFormat); // maybe it is not a problem for convertor
    }

    // Get the primitive groups
    const PrimitiveGroup* pPG = reinterpret_cast<const PrimitiveGroup*>(
        (unsigned char*)( pIH + 1 ) + pIH->nIndices_ * sourceIndices.entrySize()
    );

    // Go through the primitive groups and remap the primitves to be zero based from the start of its vertices.
    for (uint32_t i = 0; i < pIH->nTriangleGroups_; i++) {
        sourcePrimitiveGroups.push_back(pPG[i]);
        
        PrimitiveGroup& pg = sourcePrimitiveGroups.back();
        uint32_t top = 0;
        uint32_t bottom = -1;
        for (int i = 0; i < (pg.nPrimitives_ * 3); i++) {
            uint32_t val = sourceIndices[i + pg.startIndex_];
            top = max(top, val);
            bottom = min(bottom, val);
        }
        pg.nVertices_ = top - bottom + 1;
        pg.startVertex_ = bottom;
    }
}


// void loadMaterials(const XmlNode& geometryNode) {
//     for (XmlNode pgNode = geometryNode.child("primitiveGroup"); pgNode; pgNode = pgNode.next_sibling("primitiveGroup")) {
//         auto identifier = ai_trim(pgNode.read_value());
//         XmlNode materialNode = pgNode.child("material");
//     }
// }


enum ExportMode {
    NORMAL = 0,
    STATIC,
    STATIC_WITH_NODES,
    // MESH_PARTICLES
};


constexpr uint32_t BIN_SECTION_MAGIC = 0x42a14e65; // B¡Ne

std::map<std::string, UByteBuffer> introspectBinSection(const UByteBuffer& binaryData) {
    if (binaryData.size() < int(sizeof(uint32_t) + sizeof(uint32_t) * 2)) {
        throw DeadlyImportError("BW binary section has no enought data.");
    }

    char* data = (char*)&binaryData[0];
    int len = (int)binaryData.size();

    int entryDataOffset = 0;
    int entryNameLenPad = 0;
    if (*(uint32_t*)data == BIN_SECTION_MAGIC) {
        ASSIMP_LOG_DEBUG("Read MAGIC: B¡Ne");
        entryDataOffset = 4;
        entryNameLenPad = 3;
    }

    // read the index info
    int indexLen = *(int*)(data + len - sizeof(uint32_t));
    int offset = len - sizeof(uint32_t) - indexLen;

    // make sure it is valid
    if (offset < entryDataOffset || offset >= len - int(sizeof(uint32_t))) {
        throw DeadlyImportError("BW binary section has invalid index.");
    }
    ASSIMP_LOG_DEBUG("Read index info: offset ", offset, ", len ", indexLen);

    std::map<std::string, UByteBuffer> result;
    // read the directory out of the file
    while (offset <= len - (int)(sizeof(uint32_t) + sizeof(uint32_t) * 2))
    {
        // read the lengths of directory entry
        int entryDataLen = *(int*)(data + offset);
        offset += sizeof(uint32_t);
        if (((entryDataLen & (1 << 31)) || entryNameLenPad != 0) &&
            offset + sizeof(uint32_t) * 4 <= len - (sizeof(uint32_t) + sizeof(uint32_t)))
        {
            // skip extended data
            entryDataLen &= ~(1 << 31);
            // uint32_t preloadLen, uint32_t version, uint64_t modified
            offset += sizeof(uint32_t) * 4;
        }
        int entryNameLen = *(int*)(data + offset);
        offset += sizeof(int);

        // make sure they make sense
        if (uint32_t(entryDataLen) > 256 * 1024 * 1028 ||
            uint32_t(entryDataOffset + entryDataLen) > uint32_t(len - sizeof(uint32_t) - indexLen) ||
            uint32_t(entryNameLen) > 4096 ||
            uint32_t(offset + entryNameLen) > uint32_t(len - sizeof(uint32_t))) {
            throw DeadlyImportError("BW binary section contains invalid values.");
        }

        // read its name
        std::string entryStr(data + offset, entryNameLen);
        offset += (entryNameLen + entryNameLenPad) & (~entryNameLenPad);

        // add it to our list of children
        result.emplace(std::make_pair(entryStr, std::vector<unsigned char>(data + entryDataOffset, data + entryDataOffset + entryDataLen)));
        ASSIMP_LOG_DEBUG("Read entry: name ", entryStr, ", offset ", entryDataOffset, ", len ", entryDataLen);

        // move on the data offset
        entryDataOffset += (entryDataLen + 3) & (~3L);
    }

    return result;
}


struct Node: public std::enable_shared_from_this<Node> {
    aiMatrix4x4 transform_;
    aiMatrix4x4 worldTransform_;

    // int blendCookie_;
    // float blendRatio_;

// 	BlendTransform		blendTransform_;
    bool transformInBlended_;

    std::weak_ptr<Node> parent_;
    std::vector<Node> children_;

    std::string identifier_;
};

aiVector3D readVector3(const XmlNode& node, const aiVector3D& defaultVal) {
    if (node) {
        aiVector3D result;
     
        std::istringstream ss(ai_trim(node.child_value()));
        ss >> result.x >> result.y >> result.z;

        return result;
    }
    return defaultVal;
}

aiMatrix4x4 readMatrix34(const XmlNode& node, const aiMatrix4x4& defaultVal) {
    if (node) {
        aiMatrix4x4 result;

        std::istringstream row0(ai_trim(node.child_value("row0")));
        row0 >> result.a1 >> result.a2 >> result.a3;
        std::istringstream row1(ai_trim(node.child_value("row1")));
        row1 >> result.b1 >> result.b2 >> result.b3;
        std::istringstream row2(ai_trim(node.child_value("row2")));
        row2 >> result.c1 >> result.c2 >> result.c3;
        std::istringstream row3(ai_trim(node.child_value("row3")));
        row3 >> result.d1 >> result.d2 >> result.d3;

        return result;
    }
    return defaultVal;
}

aiNode* loadNodeHierarchy(const XmlNode& nodeSection, std::vector<aiNode*>& _nodes, aiNode* parent) {
    aiNode* node = new aiNode(ai_trim(nodeSection.child_value("identifier")));
    _nodes.push_back(node);
    node->mTransformation = readMatrix34(nodeSection.child("transform"), aiMatrix4x4());
    node->mParent = parent;

    // std::unique_ptr<aiNodeAnim> nodeAnim;
    vector<aiNode*> children;
    for (XmlNode childNode = nodeSection.child("node"); childNode; childNode = childNode.next_sibling("node")) {
        aiNode* child = loadNodeHierarchy(childNode, _nodes, node);
        children.emplace_back(child);
    }

    // while (ChunkSize()) {
    //     const string chunk = ReadChunk();
    //     if (chunk == "MESH") {
    //         unsigned int n = static_cast<unsigned int>(_meshes.size());
    //         ReadMESH();
    //         for (unsigned int i = n; i < static_cast<unsigned int>(_meshes.size()); ++i) {
    //             meshes.push_back(i);
    //         }
    //     } else if (chunk == "BONE") {
    //         ReadBONE(nodeid);
    //     } else if (chunk == "ANIM") {
    //         ReadANIM();
    //     } else if (chunk == "KEYS") {
    //         if (!nodeAnim) {
    //             nodeAnim.reset(new aiNodeAnim);
    //             nodeAnim->mNodeName = node->mName;
    //         }
    //         ReadKEYS(nodeAnim.get());
    //     } else if (chunk == "NODE") {
    //         aiNode *child = ReadNODE(node);
    //         children.push_back(child);
    //     }
    //     ExitChunk();
    // }

    // if (nodeAnim) {
    //     _nodeAnims.emplace_back(std::move(nodeAnim));
    // }

    if (children.empty()) {
        node->mNumChildren = 0;
        node->mChildren = nullptr;
    } else {
        node->mNumChildren = static_cast<unsigned int>(children.size());
        node->mChildren = new aiNode*[children.size()];
        std::copy(children.begin(), children.end(), node->mChildren);
    }

    return node;
}


void parseStatic(const std::string &pFile, aiScene* pScene, IOSystem* pIOHandler) {
    XmlParser xmlParser;
    {
        std::string visualFilename = pFile + ".visual";
        std::unique_ptr<IOStream> visualFile(pIOHandler->Open(visualFilename));
        // generate a XML reader for it
        if (!xmlParser.parse(visualFile.get())) {
            throw DeadlyImportError("Unable to read visual file, malformed XML");
        }
    }
    XmlNode visualFileRoot = xmlParser.getRootNode().first_child();

    std::map<std::string, UByteBuffer> sections;
    {
        std::string primitivesFilename = pFile + ".primitives";
        std::unique_ptr<IOStream> file(pIOHandler->Open(primitivesFilename));

        size_t fileSize = file->FileSize();
        if (fileSize < int(sizeof(uint32_t) + sizeof(uint32_t) * 2)) {
            throw DeadlyImportError("BW binary file is too small.");
        }

        UByteBuffer buf;
        buf.resize(fileSize);
        file->Read(&buf[0], 1, fileSize);

        sections = introspectBinSection(buf);
    }

    // load the hierarchy
    std::vector<aiNode*> nodesHolder;
    aiNode* rootNode;
    XmlNode nodeSection = visualFileRoot.child("node");
    if (nodeSection) {
        rootNode = loadNodeHierarchy(nodeSection, nodesHolder, nullptr);
    } else {
        // If there are no nodes in the resource create one.
        rootNode = new aiNode("root");
    }
    std::map<std::string, std::vector<unsigned int>> transformNodes;

    std::vector<std::unique_ptr<aiMesh>> _meshes;
    std::vector<std::unique_ptr<aiMaterial>> _materials;


    for (XmlNode renderSetNode = visualFileRoot.child("renderSet"); renderSetNode; renderSetNode = renderSetNode.next_sibling("renderSet")) {
#ifdef DEBUG_BW
        ASSIMP_LOG_DEBUG("!!! RederingSet found");
#endif
        
        // Read the node identifiers of all nodes that affect this renderset.
        std::set<std::string> nodes;
        for (XmlNode nodeIdNode = renderSetNode.child("node"); nodeIdNode; nodeIdNode = nodeIdNode.next_sibling("node")) {
            const std::string nodeId = ai_trim(nodeIdNode.child_value());
            nodes.emplace(nodeId);
        }
        if (nodes.empty()) {
            nodes.emplace(rootNode->mName.C_Str());
        }
#ifdef DEBUG_BW
        ASSIMP_LOG_DEBUG("RederingSet nodes: ", ai_to_string(nodes));
#endif

        XmlNode geometryNode = renderSetNode.child("geometry");

        std::vector<VertexXYZNUVTBPC> vertices;
        {
            std::string verticesSectionName = ai_trim(geometryNode.child_value("vertices"));
#ifdef DEBUG_BW
            ASSIMP_LOG_DEBUG("RederingSet vertices section name: ", verticesSectionName);
#endif
            auto const& verticesData = sections[verticesSectionName];
            loadVertices(verticesData, vertices); // todo: we may introduce parsed vertices cache for not to load data twice
#ifdef DEBUG_BW
            ASSIMP_LOG_DEBUG("load vertices count: ", vertices.size());
            for (auto const& v:  vertices) {
                ASSIMP_LOG_DEBUG("vertix: ", v.pos_.x, "; ", v.pos_.y, "; ", v.pos_.z, "; ");
            }
#endif
        }

        std::string primitiveSectionName = ai_trim(geometryNode.child_value("primitive"));
#ifdef DEBUG_BW
        ASSIMP_LOG_DEBUG("RederingSet primitive section name: ", primitiveSectionName);
#endif
        auto const& indicesData = sections[primitiveSectionName];

        IndicesHolder indicesHolder;
        std::vector<PrimitiveGroup> sourcePrimitiveGroups;
        loadIndices(indicesData, indicesHolder, sourcePrimitiveGroups);
        
        // Open the geometry section, the resource is assumed to be a static visual with one or more primitive groups
        for (XmlNode pgNode = geometryNode.child("primitiveGroup"); pgNode; pgNode = pgNode.next_sibling("primitiveGroup")) {
            auto identifier = ai_trim(pgNode.child_value());
            const int pgIdx{std::stoi(identifier)};

            unsigned int materialIndex = 0;
            {
                XmlNode materialNode = pgNode.child("material");

                std::unique_ptr<aiMaterial> mat(new aiMaterial);

                // Name
                aiString ainame(ai_trim(materialNode.child_value("identifier")));
                mat->AddProperty(&ainame, AI_MATKEY_NAME);

                // open the effect itself
                std::vector<std::string> fxNames;
                fxNames.emplace_back(ai_trim(materialNode.child_value("fx")));
                if (fxNames.size() > 1) {
                    ASSIMP_LOG_ERROR("Found multiple .fx files in ", pFile, ".visual");
                }

                // std::string channel = pSection->readString("channel");
                // DataSectionPtr pMFMSect = pSection->openSection("mfm");

                for (XmlNode propertyNode = materialNode.child("property"); propertyNode; propertyNode = propertyNode.next_sibling("property")) {
                    std::string property = ai_trim(propertyNode.child_value());
                    if (property == "doubleSided") {
                        std::string v(propertyNode.child_value("Bool"));
                        if (v == "true") {
                            int i = 1;
                            mat->AddProperty(&i, 1, AI_MATKEY_TWOSIDED);
                        }
                    } else if (property == "diffuseMap") {
                        // std::string filename(ai_trim(propertyNode.child_value("Texture")));
                        aiString texname(ai_trim(propertyNode.child_value("Texture")));
                        mat->AddProperty(&texname, AI_MATKEY_TEXTURE_DIFFUSE(0));
                        //
                    } else if (property == "selfIllumination") {
                        // std::string f(ai_trim(propertyNode.child_value("Float")));
                        // float v = std::stof(f);
                        // AI_MATKEY_SHININESS
                    } else if (property == "alphaTestEnable") {
                        std::string v(propertyNode.child_value("Bool"));
                        if (v == "true") {
                        }
                    } else {
                        throw DeadlyImportError("Unsupported property: ", property);
                    }
                }

                // // Diffuse color
                // mat->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);

                // // Opacity
                // mat->AddProperty(&alpha, 1, AI_MATKEY_OPACITY);

                // // Specular color
                // aiColor3D speccolor(shiny, shiny, shiny);
                // mat->AddProperty(&speccolor, 1, AI_MATKEY_COLOR_SPECULAR);

                // // Specular power
                // float specpow = shiny * 128;
                // mat->AddProperty(&specpow, 1, AI_MATKEY_SHININESS);

                // Double sided
                // if (fx & 0x10) { // doubleSided
                //     int i = 1;
                //     mat->AddProperty(&i, 1, AI_MATKEY_TWOSIDED);
                // }

                materialIndex = _materials.size();
                _materials.emplace_back(std::move(mat));
            }

            const PrimitiveGroup& pg = sourcePrimitiveGroups[pgIdx];
            std::unique_ptr<aiMesh> mesh(new aiMesh);    
            mesh->mMaterialIndex = materialIndex;
            mesh->mNumFaces = 0;
            mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

            aiFace *face = mesh->mFaces = new aiFace[pg.nPrimitives_];
            for (int32_t i = 0; i < pg.nPrimitives_; i++) {
                int pi = pg.startIndex_ + i * 3;

                int32_t i0 = indicesHolder[pi] - pg.startVertex_;
                int32_t i1 = indicesHolder[pi + 1] - pg.startVertex_;
                int32_t i2 = indicesHolder[pi + 2] - pg.startVertex_;

#ifdef DEBUG_BW
                ASSIMP_LOG_ERROR("triangle index: i0=", i0, ", i1=", i1, ", i2=", i2);
#endif
                if ((i0 < 0 || i0 >= pg.nVertices_) || (i1 < 0 || i1 >= pg.nVertices_) || (i1 < 0 || i1 >= pg.nVertices_)) {
                    throw DeadlyImportError("Incorrect trasformation node identifier: ", i0);
                }

                face->mNumIndices = 3;
                face->mIndices = new unsigned int[3];
                face->mIndices[0] = i0;
                face->mIndices[1] = i1;
                face->mIndices[2] = i2;
                ++mesh->mNumFaces;
                ++face;
            }
#ifdef DEBUG_BW
            ASSIMP_LOG_ERROR("triangle added: ", mesh->mNumFaces, " of ", pg.nPrimitives_);
#endif
            mesh->mNumVertices = pg.nVertices_;
            mesh->mVertices = new aiVector3D[mesh->mNumVertices];
            mesh->mNormals = new aiVector3D[mesh->mNumVertices];
            mesh->mTangents = new aiVector3D[mesh->mNumVertices];
            mesh->mBitangents = new aiVector3D[mesh->mNumVertices];
            mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
            
            for (int32_t idx = 0; idx < pg.nVertices_; idx++ ) {
                auto const& v = vertices[pg.startVertex_ + idx];

                mesh->mVertices[idx].x = v.pos_.x;
                mesh->mVertices[idx].y = v.pos_.y;
                mesh->mVertices[idx].z = v.pos_.z;

                mesh->mNormals[idx].x = v.normal_.x;
                mesh->mNormals[idx].y = v.normal_.y;
                mesh->mNormals[idx].z = v.normal_.z;

                mesh->mTangents[idx].x = v.tangent_.x;
                mesh->mTangents[idx].y = v.tangent_.y;
                mesh->mTangents[idx].z = v.tangent_.z;

                mesh->mBitangents[idx].x = v.binormal_.x;
                mesh->mBitangents[idx].y = v.binormal_.y;
                mesh->mBitangents[idx].z = v.binormal_.z;

                mesh->mTextureCoords[0][idx].x = v.uv_.u;
                mesh->mTextureCoords[0][idx].y = v.uv_.v;
#ifdef DEBUG_BW
                ASSIMP_LOG_ERROR("vertex index: ", idx, " (", (pg.startVertex_ + idx), "); pos: x=", mesh->mVertices[idx].x, ", y=", mesh->mVertices[idx].y, ", z=", mesh->mVertices[idx].z);
#endif
            }
#ifdef DEBUG_BW
            ASSIMP_LOG_ERROR("vertices added: ", pg.nVertices_);
#endif

            for (auto const& nodeId: nodes) {
                transformNodes[nodeId].push_back(_meshes.size());
            }

            _meshes.emplace_back(std::move(mesh));
        }
    }

            
    for (auto const& [nodeId, meshes]: transformNodes) {
        aiNode* node = rootNode->FindNode(nodeId.c_str());
        if (node == nullptr) {
            throw DeadlyImportError("Incorrect trasformation node identifier: ", nodeId);
        }
        node->mNumMeshes = static_cast<unsigned int>(meshes.size());
        node->mMeshes = to_array(meshes);
    }


    // nodes
    pScene->mRootNode = rootNode;
    // nodesHolder.clear(); // node ownership now belongs to scene

    // material
    if (!_materials.size()) {
        _materials.emplace_back(std::unique_ptr<aiMaterial>(new aiMaterial));
    }
    pScene->mNumMaterials = static_cast<unsigned int>(_materials.size());
    pScene->mMaterials = unique_to_array(_materials);

    // meshes
    pScene->mNumMeshes = static_cast<unsigned int>(_meshes.size());
    pScene->mMeshes = unique_to_array(_meshes);

    // animations
    // if (_animations.size() == 1 && _nodeAnims.size()) {

    //     aiAnimation *anim = _animations.back().get();
    //     anim->mNumChannels = static_cast<unsigned int>(_nodeAnims.size());
    //     anim->mChannels = unique_to_array(_nodeAnims);

    //     pScene->mNumAnimations = static_cast<unsigned int>(_animations.size());
    //     pScene->mAnimations = unique_to_array(_animations);
    // }

    // convert to RH
    MakeLeftHandedProcess makeleft;
    makeleft.Execute(pScene);

    FlipWindingOrderProcess flip;
    flip.Execute(pScene);

    FlipUVsProcess flipUVs;
    flipUVs.Execute(pScene);
}

// ------------------------------------------------------------------------------------------------
void BWImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    std::string ext(GetExtension(pFile));
    if (ext == "model") {
        std::unique_ptr<IOStream> file(pIOHandler->Open(pFile));

        // Check whether we can read from the file
        if (file == nullptr) {
            throw DeadlyImportError("Failed to open BW file ", pFile, ".");
        }

        XmlParser xmlParser;
        // generate a XML reader for it
        if (!xmlParser.parse(file.get())) {
            throw DeadlyImportError("Unable to read file, malformed XML");
        }
        // start reading
        XmlNode node = xmlParser.getRootNode().first_child();
        XmlNode nodelessVisual = node.child("nodelessVisual");
        if (!nodelessVisual.empty()) {
            // ExportSettings::STATIC
            std::string filename = nodelessVisual.child_value();
            filename = ai_trim(filename);
            filename = "/workspaces/assimp/test/models/BW/unit_cube"; // todo: remove it, and improve file path diagnostic
            parseStatic(filename, pScene, pIOHandler);
            return;
        }
        XmlNode nodefullVisual = node.child("nodefullVisual");
        if (!nodefullVisual.empty()) {
            // ExportSettings::NORMAL || ExportSettings::STATIC_WITH_NODES
            std::string filename = nodefullVisual.child_value();
            return;
        }

        throw DeadlyImportError("Unknown export mode in ", pFile, ".");
    } else if (ext == "visual") {
        std::string filename = pFile.substr(0, pFile.length() - ext.length() - 1);
        // todo: it may be not static file
        parseStatic(filename, pScene, pIOHandler);
        return;
    } else {
        throw DeadlyImportError("Unknown file format: ", pFile);
    }

    // _pos = 0;
    // _buf.resize(fileSize);
    // file->Read(&_buf[0], 1, fileSize);
    // _stack.clear();

    ReadBB3D(pScene);
}

// ------------------------------------------------------------------------------------------------
AI_WONT_RETURN void BWImporter::Oops() {
    throw DeadlyImportError("BW Importer - INTERNAL ERROR");
}

// ------------------------------------------------------------------------------------------------
AI_WONT_RETURN void BWImporter::Fail(const string &str) {
#ifdef DEBUG_B3D
    ASSIMP_LOG_ERROR("Error in BW file data: ", str);
#endif
    throw DeadlyImportError("BW Importer - error in B3D file data: ", str);
}

// ------------------------------------------------------------------------------------------------
int BWImporter::ReadByte() {
    if (_pos >= _buf.size()) {
        Fail("EOF");
    }

    return _buf[_pos++];
}

// ------------------------------------------------------------------------------------------------
int BWImporter::ReadInt() {
    if (_pos + 4 > _buf.size()) {
        Fail("EOF");
    }

    int n;
    memcpy(&n, &_buf[_pos], 4);
    _pos += 4;

    return n;
}

// ------------------------------------------------------------------------------------------------
float BWImporter::ReadFloat() {
    if (_pos + 4 > _buf.size()) {
        Fail("EOF");
    }

    float n;
    memcpy(&n, &_buf[_pos], 4);
    _pos += 4;

    return n;
}

// ------------------------------------------------------------------------------------------------
aiVector2D BWImporter::ReadVec2() {
    float x = ReadFloat();
    float y = ReadFloat();
    return aiVector2D(x, y);
}

// ------------------------------------------------------------------------------------------------
aiVector3D BWImporter::ReadVec3() {
    float x = ReadFloat();
    float y = ReadFloat();
    float z = ReadFloat();
    return aiVector3D(x, y, z);
}

// ------------------------------------------------------------------------------------------------
aiQuaternion BWImporter::ReadQuat() {
    // (aramis_acg) Fix to adapt the loader to changed quat orientation
    float w = -ReadFloat();
    float x = ReadFloat();
    float y = ReadFloat();
    float z = ReadFloat();
    return aiQuaternion(w, x, y, z);
}

// ------------------------------------------------------------------------------------------------
string BWImporter::ReadString() {
    if (_pos > _buf.size()) {
        Fail("EOF");
    }
    string str;
    while (_pos < _buf.size()) {
        char c = (char)ReadByte();
        if (!c) {
            return str;
        }
        str += c;
    }
    return string();
}

// ------------------------------------------------------------------------------------------------
string BWImporter::ReadChunk() {
    string tag;
    for (int i = 0; i < 4; ++i) {
        tag += char(ReadByte());
    }
#ifdef DEBUG_B3D
    ASSIMP_LOG_DEBUG("ReadChunk: ", tag);
#endif
    unsigned sz = (unsigned)ReadInt();
    _stack.push_back(_pos + sz);
    return tag;
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ExitChunk() {
    _pos = _stack.back();
    _stack.pop_back();
}

// ------------------------------------------------------------------------------------------------
size_t BWImporter::ChunkSize() {
    return _stack.back() - _pos;
}
// ------------------------------------------------------------------------------------------------

/*
template <class T>
T *BWImporter::to_array(const vector<T> &v) {
    if (v.empty()) {
        return nullptr;
    }
    T *p = new T[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        p[i] = v[i];
    }
    return p;
}

// ------------------------------------------------------------------------------------------------
template <class T>
T **unique_to_array(vector<std::unique_ptr<T>> &v) {
    if (v.empty()) {
        return nullptr;
    }
    T **p = new T *[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        p[i] = v[i].release();
    }
    return p;
}
*/

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadTEXS() {
    while (ChunkSize()) {
        string name = ReadString();
        /*int flags=*/ReadInt();
        /*int blend=*/ReadInt();
        /*aiVector2D pos=*/ReadVec2();
        /*aiVector2D scale=*/ReadVec2();
        /*float rot=*/ReadFloat();

        _textures.push_back(name);
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadBRUS() {
    int n_texs = ReadInt();
    if (n_texs < 0 || n_texs > 8) {
        Fail("Bad texture count");
    }
    while (ChunkSize()) {
        string name = ReadString();
        aiVector3D color = ReadVec3();
        float alpha = ReadFloat();
        float shiny = ReadFloat();
        /*int blend=**/ ReadInt();
        int fx = ReadInt();

        std::unique_ptr<aiMaterial> mat(new aiMaterial);

        // Name
        aiString ainame(name);
        mat->AddProperty(&ainame, AI_MATKEY_NAME);

        // Diffuse color
        mat->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);

        // Opacity
        mat->AddProperty(&alpha, 1, AI_MATKEY_OPACITY);

        // Specular color
        aiColor3D speccolor(shiny, shiny, shiny);
        mat->AddProperty(&speccolor, 1, AI_MATKEY_COLOR_SPECULAR);

        // Specular power
        float specpow = shiny * 128;
        mat->AddProperty(&specpow, 1, AI_MATKEY_SHININESS);

        // Double sided
        if (fx & 0x10) {
            int i = 1;
            mat->AddProperty(&i, 1, AI_MATKEY_TWOSIDED);
        }

        // Textures
        for (int i = 0; i < n_texs; ++i) {
            int texid = ReadInt();
            if (texid < -1 || (texid >= 0 && texid >= static_cast<int>(_textures.size()))) {
                Fail("Bad texture id");
            }
            if (i == 0 && texid >= 0) {
                aiString texname(_textures[texid]);
                mat->AddProperty(&texname, AI_MATKEY_TEXTURE_DIFFUSE(0));
            }
        }
        _materials.emplace_back(std::move(mat));
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadVRTS() {
    _vflags = ReadInt();
    _tcsets = ReadInt();
    _tcsize = ReadInt();
    if (_tcsets < 0 || _tcsets > 4 || _tcsize < 0 || _tcsize > 4) {
        Fail("Bad texcoord data");
    }

    int sz = 12 + (_vflags & 1 ? 12 : 0) + (_vflags & 2 ? 16 : 0) + (_tcsets * _tcsize * 4);
    size_t n_verts = ChunkSize() / sz;

    int v0 = static_cast<int>(_vertices.size());
    _vertices.resize(v0 + n_verts);

    for (unsigned int i = 0; i < n_verts; ++i) {
        Vertex &v = _vertices[v0 + i];

        memset(v.bones, 0, sizeof(v.bones));
        memset(v.weights, 0, sizeof(v.weights));

        v.vertex = ReadVec3();

        if (_vflags & 1) {
            v.normal = ReadVec3();
        }

        if (_vflags & 2) {
            ReadQuat(); // skip v 4bytes...
        }

        for (int j = 0; j < _tcsets; ++j) {
            float t[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < _tcsize; ++k) {
                t[k] = ReadFloat();
            }
            t[1] = 1 - t[1];
            if (!j) {
                v.texcoords = aiVector3D(t[0], t[1], t[2]);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadTRIS(int v0) {
    int matid = ReadInt();
    if (matid == -1) {
        matid = 0;
    } else if (matid < 0 || matid >= (int)_materials.size()) {
#ifdef DEBUG_B3D
        ASSIMP_LOG_ERROR("material id=", matid);
#endif
        Fail("Bad material id");
    }

    std::unique_ptr<aiMesh> mesh(new aiMesh);

    mesh->mMaterialIndex = matid;
    mesh->mNumFaces = 0;
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

    size_t n_tris = ChunkSize() / 12;
    aiFace *face = mesh->mFaces = new aiFace[n_tris];

    for (unsigned int i = 0; i < n_tris; ++i) {
        int i0 = ReadInt() + v0;
        int i1 = ReadInt() + v0;
        int i2 = ReadInt() + v0;
        if (i0 < 0 || i0 >= (int)_vertices.size() || i1 < 0 || i1 >= (int)_vertices.size() || i2 < 0 || i2 >= (int)_vertices.size()) {
#ifdef DEBUG_B3D
            ASSIMP_LOG_ERROR("Bad triangle index: i0=", i0, ", i1=", i1, ", i2=", i2);
#endif
            Fail("Bad triangle index");
        }
        face->mNumIndices = 3;
        face->mIndices = new unsigned[3];
        face->mIndices[0] = i0;
        face->mIndices[1] = i1;
        face->mIndices[2] = i2;
        ++mesh->mNumFaces;
        ++face;
    }

    _meshes.emplace_back(std::move(mesh));
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadMESH() {
    /*int matid=*/ReadInt();

    int v0 = static_cast<int>(_vertices.size());

    while (ChunkSize()) {
        string t = ReadChunk();
        if (t == "VRTS") {
            ReadVRTS();
        } else if (t == "TRIS") {
            ReadTRIS(v0);
        }
        ExitChunk();
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadBONE(int id) {
    while (ChunkSize()) {
        int vertex = ReadInt();
        float weight = ReadFloat();
        if (vertex < 0 || vertex >= (int)_vertices.size()) {
            Fail("Bad vertex index");
        }

        Vertex &v = _vertices[vertex];
        for (int i = 0; i < 4; ++i) {
            if (!v.weights[i]) {
                v.bones[i] = static_cast<unsigned char>(id);
                v.weights[i] = weight;
                break;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadKEYS(aiNodeAnim *nodeAnim) {
    vector<aiVectorKey> trans, scale;
    vector<aiQuatKey> rot;
    int flags = ReadInt();
    while (ChunkSize()) {
        int frame = ReadInt();
        if (flags & 1) {
            trans.emplace_back(frame, ReadVec3());
        }
        if (flags & 2) {
            scale.emplace_back(frame, ReadVec3());
        }
        if (flags & 4) {
            rot.emplace_back(frame, ReadQuat());
        }
    }

    if (flags & 1) {
        nodeAnim->mNumPositionKeys = static_cast<unsigned int>(trans.size());
        nodeAnim->mPositionKeys = to_array(trans);
    }

    if (flags & 2) {
        nodeAnim->mNumScalingKeys = static_cast<unsigned int>(scale.size());
        nodeAnim->mScalingKeys = to_array(scale);
    }

    if (flags & 4) {
        nodeAnim->mNumRotationKeys = static_cast<unsigned int>(rot.size());
        nodeAnim->mRotationKeys = to_array(rot);
    }
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadANIM() {
    /*int flags=*/ReadInt();
    int frames = ReadInt();
    float fps = ReadFloat();

    std::unique_ptr<aiAnimation> anim(new aiAnimation);

    anim->mDuration = frames;
    anim->mTicksPerSecond = fps;
    _animations.emplace_back(std::move(anim));
}

// ------------------------------------------------------------------------------------------------
aiNode *BWImporter::ReadNODE(aiNode *parent) {

    string name = ReadString();
    aiVector3D t = ReadVec3();
    aiVector3D s = ReadVec3();
    aiQuaternion r = ReadQuat();

    aiMatrix4x4 trans, scale, rot;

    aiMatrix4x4::Translation(t, trans);
    aiMatrix4x4::Scaling(s, scale);
    rot = aiMatrix4x4(r.GetMatrix());

    aiMatrix4x4 tform = trans * rot * scale;

    int nodeid = static_cast<int>(_nodes.size());

    aiNode *node = new aiNode(name);
    _nodes.push_back(node);

    node->mParent = parent;
    node->mTransformation = tform;

    std::unique_ptr<aiNodeAnim> nodeAnim;
    vector<unsigned> meshes;
    vector<aiNode *> children;

    while (ChunkSize()) {
        const string chunk = ReadChunk();
        if (chunk == "MESH") {
            unsigned int n = static_cast<unsigned int>(_meshes.size());
            ReadMESH();
            for (unsigned int i = n; i < static_cast<unsigned int>(_meshes.size()); ++i) {
                meshes.push_back(i);
            }
        } else if (chunk == "BONE") {
            ReadBONE(nodeid);
        } else if (chunk == "ANIM") {
            ReadANIM();
        } else if (chunk == "KEYS") {
            if (!nodeAnim) {
                nodeAnim.reset(new aiNodeAnim);
                nodeAnim->mNodeName = node->mName;
            }
            ReadKEYS(nodeAnim.get());
        } else if (chunk == "NODE") {
            aiNode *child = ReadNODE(node);
            children.push_back(child);
        }
        ExitChunk();
    }

    if (nodeAnim) {
        _nodeAnims.emplace_back(std::move(nodeAnim));
    }

    node->mNumMeshes = static_cast<unsigned int>(meshes.size());
    node->mMeshes = to_array(meshes);

    node->mNumChildren = static_cast<unsigned int>(children.size());
    node->mChildren = to_array(children);

    return node;
}

// ------------------------------------------------------------------------------------------------
void BWImporter::ReadBB3D(aiScene *scene) {

    _textures.clear();

    _materials.clear();

    _vertices.clear();

    _meshes.clear();

    DeleteAllBarePointers(_nodes);
    _nodes.clear();

    _nodeAnims.clear();

    _animations.clear();

    string t = ReadChunk();
    if (t == "BB3D") {
        int version = ReadInt();

        if (!DefaultLogger::isNullLogger()) {
            char dmp[128];
            ai_snprintf(dmp, 128, "B3D file format version: %i", version);
            ASSIMP_LOG_INFO(dmp);
        }

        while (ChunkSize()) {
            const string chunk = ReadChunk();
            if (chunk == "TEXS") {
                ReadTEXS();
            } else if (chunk == "BRUS") {
                ReadBRUS();
            } else if (chunk == "NODE") {
                ReadNODE(nullptr);
            }
            ExitChunk();
        }
    }
    ExitChunk();

    if (!_nodes.size()) {
        Fail("No nodes");
    }

    if (!_meshes.size()) {
        Fail("No meshes");
    }

    // Fix nodes/meshes/bones
    for (size_t i = 0; i < _nodes.size(); ++i) {
        aiNode *node = _nodes[i];

        for (size_t j = 0; j < node->mNumMeshes; ++j) {
            aiMesh *mesh = _meshes[node->mMeshes[j]].get();

            int n_tris = mesh->mNumFaces;
            int n_verts = mesh->mNumVertices = n_tris * 3;

            aiVector3D *mv = mesh->mVertices = new aiVector3D[n_verts], *mn = nullptr, *mc = nullptr;
            if (_vflags & 1) {
                mn = mesh->mNormals = new aiVector3D[n_verts];
            }
            if (_tcsets) {
                mc = mesh->mTextureCoords[0] = new aiVector3D[n_verts];
            }

            aiFace *face = mesh->mFaces;

            vector<vector<aiVertexWeight>> vweights(_nodes.size());

            for (int vertIdx = 0; vertIdx < n_verts; vertIdx += 3) {
                for (int faceIndex = 0; faceIndex < 3; ++faceIndex) {
                    Vertex &v = _vertices[face->mIndices[faceIndex]];

                    *mv++ = v.vertex;
                    if (mn) *mn++ = v.normal;
                    if (mc) *mc++ = v.texcoords;

                    face->mIndices[faceIndex] = vertIdx + faceIndex;

                    for (int k = 0; k < 4; ++k) {
                        if (!v.weights[k])
                            break;

                        int bone = v.bones[k];
                        float weight = v.weights[k];

                        vweights[bone].emplace_back(vertIdx + faceIndex, weight);
                    }
                }
                ++face;
            }

            vector<aiBone *> bones;
            for (size_t weightIndx = 0; weightIndx < vweights.size(); ++weightIndx) {
                vector<aiVertexWeight> &weights = vweights[weightIndx];
                if (!weights.size()) {
                    continue;
                }

                aiBone *bone = new aiBone;
                bones.push_back(bone);

                aiNode *bnode = _nodes[weightIndx];

                bone->mName = bnode->mName;
                bone->mNumWeights = static_cast<unsigned int>(weights.size());
                bone->mWeights = to_array(weights);

                aiMatrix4x4 mat = bnode->mTransformation;
                while (bnode->mParent) {
                    bnode = bnode->mParent;
                    mat = bnode->mTransformation * mat;
                }
                bone->mOffsetMatrix = mat.Inverse();
            }
            mesh->mNumBones = static_cast<unsigned int>(bones.size());
            mesh->mBones = to_array(bones);
        }
    }

    // nodes
    scene->mRootNode = _nodes[0];
    _nodes.clear(); // node ownership now belongs to scene

    // material
    if (!_materials.size()) {
        _materials.emplace_back(std::unique_ptr<aiMaterial>(new aiMaterial));
    }
    scene->mNumMaterials = static_cast<unsigned int>(_materials.size());
    scene->mMaterials = unique_to_array(_materials);

    // meshes
    scene->mNumMeshes = static_cast<unsigned int>(_meshes.size());
    scene->mMeshes = unique_to_array(_meshes);

    // animations
    if (_animations.size() == 1 && _nodeAnims.size()) {

        aiAnimation *anim = _animations.back().get();
        anim->mNumChannels = static_cast<unsigned int>(_nodeAnims.size());
        anim->mChannels = unique_to_array(_nodeAnims);

        scene->mNumAnimations = static_cast<unsigned int>(_animations.size());
        scene->mAnimations = unique_to_array(_animations);
    }

    // convert to RH
    MakeLeftHandedProcess makeleft;
    makeleft.Execute(scene);

    FlipWindingOrderProcess flip;
    flip.Execute(scene);
}

} // namespace Assimp

#endif // !! ASSIMP_BUILD_NO_BW_IMPORTER
