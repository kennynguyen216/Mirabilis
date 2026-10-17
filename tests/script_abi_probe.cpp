#include "script_host.h"
#include <cstdlib>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3)return 2;
#ifdef _WIN32
    _putenv_s("MIRABILIS_BAD_MANAGED_ABI",argv[2]);
#else
    setenv("MIRABILIS_BAD_MANAGED_ABI",argv[2],1);
#endif
    mirabilis::Scene scene;mirabilis::PhysicsWorld physics;mirabilis::ScriptHost host;
    const auto entity=scene.create_object("retained");
    const Script saved{"Missing.Assembly","Missing.Type",true,{{"Preserved",{"string",R"("saved")"}}}};
    if(!scene.set(entity,saved))return 3;
    host.bind(scene,physics);
    if(host.initialize(argv[1]) || host.available() || host.error().empty()) {
        std::cerr<<"Bad managed "<<argv[2]<<" table was not rejected: "<<host.error()<<'\n';return 1;
    }
    host.start();host.update(.01f);host.fixed_update(.01f);host.stop();
    if(host.live_count()!=0 || !scene.read<Script>(entity) || *scene.read<Script>(entity)!=saved || !scene.valid(entity))return 1;
    std::cout<<"Native managed-ABI rejection PASS: "<<argv[2]<<"; "<<host.error()<<'\n';return 0;
}
