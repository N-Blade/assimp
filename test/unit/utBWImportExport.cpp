#include "AbstractImportExportBase.h"
#include "UnitTestPCH.h"

#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>

using namespace Assimp;

class utBWImportExport : public AbstractImportExportBase {
public:
    virtual bool importerTest() {
        Assimp::Importer importer;
//        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/BW/unit_cube.model", aiProcess_ValidateDataStructure);
        const aiScene *scene = importer.ReadFile("/game/res/models/misc/S1_192_Flag_v01.visual", aiProcess_ValidateDataStructure);
        return nullptr != scene;
    }
};

TEST_F(utBWImportExport, importACFromFileTest) {
    EXPECT_TRUE(importerTest());
}
