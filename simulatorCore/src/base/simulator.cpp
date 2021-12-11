/*
 * simulator.cpp
 *
 *  Created on: 22 mars 2013
 *      Author: dom
 */

#include "simulator.h"

#include <algorithm>
#include <climits>
#include <unordered_set>

#include "../grid/cell3DPosition.h"
#include "../utils/trace.h"
#include "../utils/utils.h"
#include "../utils/global.h"
#include "../meld/meldInterpretVM.h"
#include "../meld/meldInterpretScheduler.h"
#include "../events/cppScheduler.h"
#include "../gui/openglViewer.h"
#include "../grid/target.h"
#include "../csg/csg.h"
#include "../csg/csgParser.h"
#include "../replay/replayExporter.h"

using namespace std;

namespace BaseSimulator {

Simulator* simulator = nullptr;

Simulator* Simulator::simulator = nullptr;

Simulator::Type	Simulator::type = CPP; // CPP code by default
bool Simulator::regrTesting = false; // No regression testing by default

Simulator::Simulator(int argc, char *argv[], BlockCodeBuilder _bcb): bcb(_bcb), cmdLine(argc,argv, _bcb) {
#ifdef DEBUG_OBJECT_LIFECYCLE
    OUTPUT << TermColor::LifecycleColor << "Simulator constructor" << TermColor::Reset << endl;
#endif

    // Ensure that only one instance of simulator is running at once
    if (simulator == nullptr) {
        simulator = this;
        BaseSimulator::simulator = simulator;
    } else {
        ERRPUT << TermColor::ErrorColor << "Only one Simulator instance can be created, aborting !"
               << TermColor::Reset << endl;
        exit(EXIT_FAILURE);
    }

    // Ensure that the configuration file exists and is well-formed

    string confFileName = cmdLine.getConfigFile();

    xmlDoc = new TiXmlDocument(confFileName.c_str());
    bool isLoaded = xmlDoc->LoadFile();

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(1,INT_MAX); // [1,intmax]
    if (cmdLine.isSimulationSeedSet()) {
        seed = cmdLine.getSimulationSeed();
    } else {
        // Set random seed
        seed = dis(gen);
    }

    generator = uintRNG((ruint)seed);

    cerr << TermColor::BWhite << "Simulation Seed: " << seed << TermColor::Reset << endl;

    if (!isLoaded) {
        cerr << "error: Could not load configuration file: " << confFileName << endl;
        exit(EXIT_FAILURE);
    } else {
        xmlWorldNode = xmlDoc->FirstChild("world");
        if (xmlWorldNode) {
#ifdef DEBUG_CONF_PARSING
            OUTPUT << "\033[1;34m  " << confFileName << " successfully loaded "
                   << TermColor::Reset << endl;
#endif
        } else {
            ERRPUT << TermColor::ErrorColor << "error: Could not find root 'world' element in configuration file"
                   << TermColor::Reset << endl;
            exit(EXIT_FAILURE);
        }
    }
}

Simulator::~Simulator() {
#ifdef DEBUG_OBJECT_LIFECYCLE
    OUTPUT << TermColor::LifecycleColor  << "Simulator destructor" << TermColor::Reset << endl;
#endif
    delete xmlDoc;
    if (ReplayExporter::isReplayEnabled())
        delete ReplayExporter::getInstance();

    deleteWorld();
}

void Simulator::deleteSimulator() {
    delete simulator;
    simulator = nullptr;
}

void Simulator::loadScheduler(int schedulerMaxDate) {
    int sl = cmdLine.getSchedulerLength();
    int sm = cmdLine.getSchedulerMode();

    // Create a scheduler of the same type as target BlockCode
    switch(getType()) {
        case MELDINTERPRET:
            MeldInterpret::MeldInterpretScheduler::createScheduler();
            break;
        case MELDPROCESS:
#ifdef MELDPROCESSVM
            MeldProcess::MeldProcessScheduler::createScheduler();
#else
            cerr << "error: MeldProcess not compiled in this version. "
                 << "Please enable MELDPROCESS in simulatorCore/src/Makefile to enable it, and try again"
                 << endl;
            exit(EXIT_FAILURE);
#endif
            break;
        case CPP:
            CPPScheduler::createScheduler();
            break;
    }

    scheduler = getScheduler();

    // Set the scheduler execution mode on start, if enabled
    if (sm != CMD_LINE_UNDEFINED) {
        if (!GlutContext::GUIisEnabled && sm == SCHEDULER_MODE_REALTIME) {
            cerr << "error: Realtime mode cannot be used when in terminal mode" << endl;
            exit(EXIT_FAILURE);
        }

        scheduler->setSchedulerMode(sm);
        scheduler->setAutoStart(true);
    }

    if (!GlutContext::GUIisEnabled) {
        // If GUI disabled, and no mode specified, set fastest mode by default (Normally REALTIME)
        scheduler->setSchedulerMode(SCHEDULER_MODE_FASTEST);
    }

    // Set the scheduler termination mode
    scheduler->setSchedulerLength(sl);
    scheduler->setAutoStop(cmdLine.getSchedulerAutoStop());

    if (sl == SCHEDULER_LENGTH_BOUNDED) {
        scheduler->setMaximumDate(cmdLine.getMaximumDate());
    }
}

void Simulator::parseConfiguration(int argc, char*argv[]) {
    try {
        // Identify the type of the simulation (CPP / Meld Process / MeldInterpret)
        readSimulationType(argc, argv);

        // Configure the simulation world
        parseWorld(argc, argv);
        initializeIDPool();

        // Instantiate and configure the Scheduler
        loadScheduler(schedulerMaxDate);

        // Parse and configure the remaining items
        parseBlockList();
        parseCameraAndSpotlight();
        parseObstacles();
        parseTarget();
        parseCustomizations();
    } catch(ParsingException const& e) {
        cerr << e.what();
        exit(EXIT_FAILURE);
    }
}

Simulator::IDScheme Simulator::determineIDScheme() {
    TiXmlElement* element = xmlBlockListNode->ToElement();
    const char *attr= element->Attribute("ids");
    if (attr) {
        string str(attr);

        if (str.compare("MANUAL") == 0)
            return MANUAL;
        else if (str.compare("ORDERED") == 0)
            return ORDERED;
        else if (str.compare("RANDOM") == 0)
            return RANDOM;
        else {
            stringstream error;
            error << "unknown ID distribution scheme in configuration file: "
                  << str << "\n";
            error << "\texpected values: [ORDERED, MANUAL, RANDOM]" << "\n";
            throw ParsingException(error.str());
        }
    }

    return ORDERED;
}

// Seed for ID generation:
// USES: idseed blocklist XML attribute if specified,
//       OR otherwise, simulation seed if specified,
//       OR otherwise, a random seed
int Simulator::parseRandomIdSeed() {
    TiXmlElement *element = xmlBlockListNode->ToElement();
    const char *attr = element->Attribute("idseed");
    if (attr) {				// READ Seed
        try {
            string str(attr);
            return stoi(str);
        } catch (const std::invalid_argument& e) {
            stringstream error;
            error << "invalid seed attribute value in configuration file: " << attr << "\n";
            throw ParsingException(error.str());
        }
    } else { // No seed, generate distribution with random seed or cmd line seed
        return cmdLine.isSimulationSeedSet() ? cmdLine.getSimulationSeed() : -1;
    }
}

bID Simulator::parseRandomStep() {
    TiXmlElement *element = xmlBlockListNode->ToElement();
    const char *attr = element->Attribute("step");
    if (attr) {				// READ Step
        try {
            string str(attr);
            return stol(str);
        } catch (const std::invalid_argument& e) {
            stringstream error;
            error << "invalid step attribute value in configuration file: " << attr << "\n";
            throw ParsingException(error.str());
        }
    } else {				// No step, generate distribution with step of one
        return 1;
    }
}

//!< std::iota does not support a step for filling the container.
//!< Hence, we use this template wrapper to overload the ++ operator
//!< cf: http://stackoverflow.com/a/34545507/3582770
template <class T>
struct IotaWrapper {
    typedef T type;

    type value;
    type step;

    IotaWrapper() = delete;
    IotaWrapper(const type& n, const type& s) : value(n), step(s) {};

    operator type() { return value; }
    IotaWrapper& operator++() { value += step; return *this; }
};

void Simulator::generateRandomIDs(const int n, const int idSeed, const int step) {
    // Fill vector with n numbers from 0 and with a distance of step to each other
    IotaWrapper<bID> inc(1, step);
    IDPool = vector<bID>(n);
    std::iota(begin(IDPool), end(IDPool), inc);

    // Seed for ID generation:
    // USES: idseed blocklist XML attribute if specified,
    //       OR otherwise, simulation seed if specified,
    //       OR otherwise, a random seed

    // Properly seed random number generator
    std::mt19937 gen;
    if (idSeed == -1) {
        OUTPUT << "Generating fully random contiguous ID distribution" <<  endl;
        gen = std::mt19937(seed);
        cerr << TermColor::BWhite << "ID Seed: " << seed << TermColor::Reset << endl;
    } else {
        OUTPUT << "Generating random contiguous ID distribution with seed: "
               << idSeed <<  endl;
        gen = std::mt19937(idSeed);
        cerr << TermColor::BWhite << "ID Seed: " << idSeed << TermColor::Reset << endl;
    }

    // Shuffle the elements using the rng
    std::shuffle(begin(IDPool), end(IDPool), gen);
}

bID Simulator::countNumberOfModules() {
    TiXmlElement *element;
    const char *attr;
    bID moduleCount = 0;

    // Count modules from block elements
    for(TiXmlNode *child = xmlBlockListNode->FirstChild("block"); child; child = child->NextSibling("block")){

        // by hussein
        const char* obst;
        TiXmlElement *element;
        element = child->ToElement();
        obst = element->Attribute("obstacle");
        if(!attr){
            moduleCount++;
        }
        
    }
        

    // Count modules from blocksLine elements
    for(TiXmlNode *child = xmlBlockListNode->FirstChild("blocksLine"); child; child = child->NextSibling("blocksLine")) {
        element = child->ToElement();
        attr = element->Attribute("values");
        if (attr) {
            string str(attr);
            int n = str.length();
            for(int i = 0; i < n; i++) {
                if  (str[i]=='1')
                    moduleCount++;
            }
        }
    }

    // cout << "count" << endl;
    // Count modules from blockBox elements
    for(TiXmlNode *child = xmlBlockListNode->FirstChild("blockBox"); child; child = child->NextSibling("blockBox")) {
        Vector3D boxOrigin(0,0,0);
        element = child->ToElement();
        attr = element->Attribute("boxOrigin");
        if (attr) {
            cout << "origin" << endl;
            string str(attr);
            int pos1 = str.find_first_of(','),
  pos2 = str.find_last_of(',');
            boxOrigin.pt[0] = stof(str.substr(0,pos1));
            boxOrigin.pt[1] = stof(str.substr(pos1+1,pos2-pos1-1));
            boxOrigin.pt[2] = stof(str.substr(pos2+1,str.length()-pos1-1));
        }
        Vector3D boxDest(world->lattice->gridSize[0]*world->lattice->gridScale[0],
                         world->lattice->gridSize[1]*world->lattice->gridScale[1],
                         world->lattice->gridSize[2]*world->lattice->gridScale[2]);
        attr = element->Attribute("boxSize");
        if (attr) {
            string str(attr);
            int pos1 = str.find_first_of(','),
                pos2 = str.find_last_of(',');
            boxDest.pt[0] = boxOrigin.pt[0] + stof(str.substr(0,pos1));
            boxDest.pt[1] = boxOrigin.pt[1] + stof(str.substr(pos1+1,pos2-pos1-1));
            boxDest.pt[2] = boxOrigin.pt[2] + stof(str.substr(pos2+1,str.length()-pos1-1));
            cout << "boxDest" << endl;
        }
        Vector3D pos;
        Cell3DPosition position;
        for (short iz=0; iz<world->lattice->gridSize[2]; iz++) {
            for (short iy=0; iy<world->lattice->gridSize[1]; iy++) {
                for (short ix=0; ix<world->lattice->gridSize[0]; ix++) {
                    position.set(ix,iy,iz);
                    pos = world->lattice->gridToWorldPosition(position);
                    if (pos.isInBox(boxOrigin,boxDest)) {
                        moduleCount++;
                    }
                }
            }
        }
    }
    // cout << "count=" << moduleCount << endl;

    return moduleCount;
}

void Simulator::initializeIDPool() {
    // Simulator.xmlBlockListNode has to be initialized at this point!

    // Determine what assignment model to use, and initialize IDPool according to it
    ids = determineIDScheme();

    // DO NOT initialize IDPool if ORDERED,
    //  it is unecessary and requires additional computation
    //  (Additional reading of the configuration file)
    if (ids == ORDERED) return;

    // Count number of modules in configuration file
    bID numModules = countNumberOfModules();
    cerr << "There are " << numModules << " modules in the configuration" << endl;

    switch (ids) {
        case ORDERED:
            // Fill IDPool with ID {1..N}
            // IDPool = vector<bID>(numModules);
            // std::iota(begin(IDPool), end(IDPool), 1);

            // DO NOT initialize IDPool if ORDERED,
            //  it is unecessary and requires additional computation
            //  (Additional reading of the configuration file)
            break;
        case MANUAL: {
            bID id;
            TiXmlElement *element;
            const char *attr;
            unordered_set<int> dupCheck;			// Set containing all previously assigned IDs, used to check for duplicates
            for(TiXmlNode *child = xmlBlockListNode->FirstChild("block"); child; child = child->NextSibling("block")) {
                element = child->ToElement();
                
                //by hussein
                attr=element->Attribute("obstacle");
                if(!attr)
                {
                    attr = element->Attribute("id");

                    if (attr) {
                        try {
                            string str(attr);
                            id =  stoull(str); // id in range [0, 2^64 - 1]
                        } catch (const std::invalid_argument& e) {
                            stringstream error;
                            error << "invalid id attribute value in configuration file: "
                                << attr << "\n";
                            throw ParsingException(error.str());
                        } catch (const std::out_of_range& e) {
                            stringstream error;
                            error << "out of range id attribute value in configuration file: "
                                << attr << "\n";
                            throw ParsingException(error.str());
                        }
                    } else {
                        stringstream error;
                        error << "missing id attribute for block node in configuration file while in MANUAL mode" << "\n";
                        throw ParsingException(error.str());
                    }

                    // Ensure unicity of the ID, by inserting id to the set and checking that insertion took place
                    //  (insertion does not take place if there is a duplicate, and false is returned)
                    if (dupCheck.insert(id).second)
                        IDPool.push_back(id);
                    else {
                        stringstream error;
                        error << "duplicate id attribute " << id << " for block node in configuration file while in MANUAL mode" << "\n";
                        throw ParsingException(error.str());
                    }

                }
            }

        } break;
        case RANDOM:
            generateRandomIDs(numModules, parseRandomIdSeed(), parseRandomStep());
            break;
    } // switch

    // cerr << "{";
    // for (bID id : IDPool)
    //  cerr << id << ", ";
    // cerr << "}" << endl;
}

void Simulator::readSimulationType(int argc, char*argv[]) {
    if(getType() == MELDINTERPRET) {
        string programPath = cmdLine.getProgramPath();
        bool debugging = cmdLine.getMeldDebugger();

        if (!file_exists(programPath)) {
            cerr << "error: no Meld program was provided, or default file \"./program.bb\" does not exist"
                 << endl;
            exit(1);
        }
        OUTPUT << "Loading " << programPath << " with MeldInterpretVM" << endl;
        MeldInterpret::MeldInterpretVM::setConfiguration(programPath, debugging);
        if(debugging){
            //Don't know what to do yet
            cerr << "warning: MeldInterpreter debugging not implemented yet" << endl;
        }
    }
}

void Simulator::parseWorld(int argc, char*argv[]) {
    TiXmlNode *xmlVisualNode = xmlDoc->FirstChild("visuals");
    if (xmlVisualNode) {
        TiXmlElement* visualElement = xmlVisualNode->ToElement();
        const char *attr= visualElement->Attribute("windowSize");
        if (attr) {
            string str=attr;
            int pos = str.find_first_of(",x");
            GlutContext::initialScreenWidth = stoi(str.substr(0,pos));
            GlutContext::initialScreenHeight = stoi(str.substr(pos+1,str.length()-pos-1));
            GlutContext::screenWidth = GlutContext::initialScreenWidth;
            GlutContext::screenHeight = GlutContext::initialScreenHeight;
        }

        attr= visualElement->Attribute("enableShadows");
        if (attr) {
            string str(attr);
            toLowercase(str);
            GlutContext::enableShadows = (str=="true" || str=="yes");
        }

        attr= visualElement->Attribute("showGrid");
        if (attr) {
            string str(attr);
            toLowercase(str);
            GlutContext::showGrid = (str=="true" || str=="yes");
            cout << "showGrid:" << str<< ": "<<GlutContext::showGrid << endl;
        }

        attr= visualElement->Attribute("backgroundColor");
        if (attr) {
            string str(attr);
            size_t pos=str.find_first_of('#');
            str.replace(pos,1,"0x");
            unsigned int c = stoul(str, nullptr, 16);
            GlutContext::bgColor[0] = static_cast<float>(c/65236)/256.0f;
            GlutContext::bgColor[1] = static_cast<float>((c/256)%256)/256.0f;
            GlutContext::bgColor[2] = static_cast<float>(c%256)/256.0f;
            //cout << str << "=" << GlutContext::bgColor[0] << "," << GlutContext::bgColor[1] << "," << GlutContext::bgColor[2] << endl;
        }

        attr= visualElement->Attribute("backgroundGradientColor");
        if (attr) {
            GlutContext::hasGradientBackground = true;
            string str(attr);
            size_t pos=str.find_first_of('#');
            str.replace(pos,1,"0x");
            unsigned int c = stoul(str, nullptr, 16);
            GlutContext::bgColor2[0] = static_cast<float>(c/65236)/256.0f;
            GlutContext::bgColor2[1] = static_cast<float>((c/256)%256)/256.0f;
            GlutContext::bgColor2[2] = static_cast<float>(c%256)/256.0f;
            //cout << str << "=" << GlutContext::bgColor[0] << "," << GlutContext::bgColor[1] << "," << GlutContext::bgColor[2] << endl;
        }

    }
    /* reading the xml file */
    xmlWorldNode = xmlDoc->FirstChild("world");

    if (xmlWorldNode) {
        TiXmlElement* worldElement = xmlWorldNode->ToElement();
        const char *attr= worldElement->Attribute("gridSize");

        int lx = 0;
        int ly = 0;
        int lz = 0;

        if (attr) {
            string str=attr;
            int pos1 = str.find_first_of(','),
                pos2 = str.find_last_of(',');
            lx = stoi(str.substr(0,pos1));
            ly = stoi(str.substr(pos1+1,pos2-pos1-1));
            lz = stoi(str.substr(pos2+1,str.length()-pos1-1));
#ifdef DEBUG_CONF_PARSING
            OUTPUT << "grid size : " << lx << " x " << ly << " x " << lz << endl;
#endif
        } else {
#ifdef DEBUG_CONF_PARSING
            OUTPUT << "warning: No grid size in XML file" << endl;
#endif
        }

        attr = worldElement->Attribute("windowSize");
        if (attr) {
            string str=attr;
            int pos = str.find_first_of(',');
            GlutContext::initialScreenWidth = stoi(str.substr(0,pos));
            GlutContext::initialScreenHeight = stoi(str.substr(pos+1,str.length()-pos-1));
            GlutContext::screenWidth = GlutContext::initialScreenWidth;
            GlutContext::screenHeight = GlutContext::initialScreenHeight;
            cerr << "warning [DEPRECATED]: place windowSize in visuals tag!" << endl;
        }

        attr=worldElement->Attribute("maxSimulationTime");
        if (attr) {
            string str=attr;
            Time t = atol(attr);
            int l = strlen(attr);
            if (str.substr(l-2,2)=="mn") {
                t*=60000000;
            } else if (str.substr(l-2,2)=="ms") {
                t*=1000;
            } else if (str.substr(l-1,1)=="s") {
                t*=1000000;
            }

            schedulerMaxDate = t;
            cerr << "warning: maxSimulationTime in the configuration is not supported anymore,"
                 << " please use the command line option [-s <maxTime>]" << endl;
        }

//         // Get Blocksize
//         float blockSize[3] = {0.0,0.0,0.0};
        xmlBlockListNode = xmlWorldNode->FirstChild("blockList");
//         // if (xmlBlockListNode) {
//             TiXmlElement *blockListElement = xmlBlockListNode->ToElement();
//             attr = blockListElement->Attribute("blockSize");
//             if (attr) {
//                 string str(attr);
//                 int pos1 = str.find_first_of(','),
//                     pos2 = str.find_last_of(',');
//                 blockSize[0] = atof(str.substr(0,pos1).c_str());
//                 blockSize[1] = atof(str.substr(pos1+1,pos2-pos1-1).c_str());
//                 blockSize[2] = atof(str.substr(pos2+1,str.length()-pos1-1).c_str());
// #ifdef DEBUG_CONF_PARSING
//                 OUTPUT << "blocksize =" << blockSize[0] << "," << blockSize[1] << "," << blockSize[2] << endl;
// #endif
//             }
        if (not xmlBlockListNode)  {
            stringstream error;
            error << "No blockList element in XML configuration file" << "\n";
            throw ParsingException(error.str());
        }

        // Create the simulation world and lattice
        loadWorld(Cell3DPosition(lx,ly,lz),
                  Vector3D(0, 0, 0), // Always use default blocksize
                  argc, argv);
    } else {
        stringstream error;
        error << "No world in XML configuration file" << "\n";
        throw ParsingException(error.str());
    }
}

void Simulator::parseCameraAndSpotlight() {
    if (GlutContext::GUIisEnabled) {
        Lattice *lattice = getWorld()->lattice;
        // calculate the position of the camera from the lattice size
        Vector3D target(0.5*lattice->gridSize[0]*lattice->gridScale[0],
                                        0.5*lattice->gridSize[1]*lattice->gridScale[1],
                                        0.25*lattice->gridSize[2]*lattice->gridScale[2]); // usual target point (midx,miy,quarterz)
        world->getCamera()->setTarget(target);
        double d=target.norme();
        world->getCamera()->setDistance(3.0*d);
        world->getCamera()->setDirection(-30.0-90.0,30.0);
        world->getCamera()->setNearFar(0.25*d,5.0*d);
        world->getCamera()->setAngle(35.0);
        world->getCamera()->setLightParameters(target,-30.0,30.0,3.0*d,30.0,0.25*d,4.0*d);
        // loading the camera parameters
        TiXmlNode *nodeConfig = xmlWorldNode->FirstChild("camera");
        if (nodeConfig) {
            TiXmlElement* cameraElement = nodeConfig->ToElement();
            const char *attr=cameraElement->Attribute("target");
            double def_near=1,def_far=1500;
            float angle=45.0;
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                Vector3D target;
                target.pt[0] = stof(str.substr(0,pos1));
                target.pt[1] = stof(str.substr(pos1+1,pos2-pos1-1));
                target.pt[2] = stof(str.substr(pos2+1,str.length()-pos1-1));
                world->getCamera()->setTarget(target);
            }

            attr=cameraElement->Attribute("angle");
            if (attr) {
                angle = atof(attr);
                world->getCamera()->setAngle(angle);
            }

            attr=cameraElement->Attribute("directionSpherical");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                float az,ele,dist;
                az = -90.0f+stof(str.substr(0,pos1));
                ele = stof(str.substr(pos1+1,pos2-pos1-1));
                dist = stof(str.substr(pos2+1,str.length()-pos1-1));
                world->getCamera()->setDirection(az,ele);
                world->getCamera()->setDistance(dist);
                // az = dist*sin(angle*M_PI/180.0);
                // def_near = dist-az;
                // def_far = dist+az;
            }

            attr=cameraElement->Attribute("near");
            if (attr) {
                def_near = strtod(attr, nullptr);
            }

            attr=cameraElement->Attribute("far");
            if (attr) {
                def_far = strtod(attr, nullptr);
            }
            world->getCamera()->setNearFar(def_near,def_far);
        }

        // loading the spotlight parameters
        nodeConfig = xmlWorldNode->FirstChild("spotlight");
        if (nodeConfig) {
            Vector3D target;
            float az=0,ele=60,dist=1000,angle=50;
            double nearPlane=10,farPlane=2000;
            TiXmlElement* lightElement = nodeConfig->ToElement();
            const char *attr=lightElement->Attribute("target");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                target.pt[0] = stof(str.substr(0,pos1));
                target.pt[1] = stof(str.substr(pos1+1,pos2-pos1-1));
                target.pt[2] = stof(str.substr(pos2+1,str.length()-pos1-1));
            }

            attr=lightElement->Attribute("directionSpherical");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                az = -90.0+stof(str.substr(0,pos1));
                ele = stof(str.substr(pos1+1,pos2-pos1-1));
                dist = stof(str.substr(pos2+1,str.length()-pos1-1));
            }

            attr=lightElement->Attribute("angle");
            if (attr) {
                angle = atof(attr);
            }

            world->getCamera()->getNearFar(nearPlane,farPlane);
            attr=lightElement->Attribute("near");
            if (attr) {
                nearPlane = atof(attr);
            }

            attr=lightElement->Attribute("far");
            if (attr) {
                farPlane = atof(attr);
            }
            world->getCamera()->setLightParameters(target,az,ele,dist,angle,nearPlane,farPlane);
        }
    }
}

void Simulator::parseBlockList() {
    int indexBlock = 0;

    TiXmlElement* element = xmlBlockListNode->ToElement();
    if (xmlBlockListNode) {
        Color defaultColor = DARKGREY;
        const char *attr= element->Attribute("color");
        defaultColor.set(attr);

#ifdef DEBUG_CONF_PARSING
        if (attr) {
            OUTPUT << "new default color :" << defaultColor << endl;
        }
#endif
        /* Reading a catoms */
        TiXmlNode *block = xmlBlockListNode->FirstChild("block");
        Cell3DPosition position;
        Color color;
        bool master;
        while (block) {
            element = block->ToElement();
            color=defaultColor;
            master=false;
            attr = element->Attribute("color");
            color.set(attr);
#ifdef DEBUG_CONF_PARSING
            if (attr) {
                OUTPUT << "new color :" << defaultColor << endl;
            }
#endif
            attr = element->Attribute("position");
            if (attr) {
                string str(attr);
                int pos = str.find_first_of(',');
                int pos2 = str.find_last_of(',');
                int ix = stoi(str.substr(0,pos)),
                    iy = stoi(str.substr(pos+1,pos2-pos-1)),
                    iz = stoi(str.substr(pos2+1,str.length()-pos2-1));

                // cerr << ix << "," << iy << "," << iz << endl;

                position.pt[0] = ix;
                position.pt[1] = iy;
                position.pt[2] = iz;
            }
            attr = element->Attribute("master");
            if (attr) {
                string str(attr);
                if (str.compare("true")==0 || str.compare("1")==0) {
                    master=true;
                }
#ifdef DEBUG_CONF_PARSING
                OUTPUT << "master : " << master << endl;
#endif
            }

            if (not getWorld()->lattice->isInGrid(position)) {
                cerr << "GridLowerBounds: "
                     << getWorld()->lattice->getGridLowerBounds(position[2]) << endl;
                cerr << "GridUpperBounds: "
                     << getWorld()->lattice->getGridUpperBounds(position[2])<< endl;
                stringstream error;
                error << "module at " << position << " is out of grid" << "\n";
                throw ParsingException(error.str());
            }
            
            //by hussein
            const char* obst;
            TiXmlElement *element;
            element = block->ToElement();
            obst = element->Attribute("obstacle");
            if(!obst){
                // cerr << "addBlock(" << currentID << ") pos = " << position << endl;
                loadBlock(element, ids == ORDERED ? ++indexBlock:IDPool[indexBlock++], bcb, position, color, master);
            }
            else{
                int orientation = 0 ;
                attr = element->Attribute("orientation");
                if(attr){
                    orientation = atoi(attr);
                }
                loadObstacle(ids == ORDERED ? ++indexBlock:IDPool[indexBlock++], bcb, position, color, orientation, master);

            }


            block = block->NextSibling("block");
        } // end while (block)

        // Reading blocks lines
        block = xmlBlockListNode->FirstChild("blocksLine");
        int line = 0, plane = 0;
        while (block) {
            if (ids == MANUAL) {
                stringstream error;
                error << "blocksLine element cannot be used in MANUAL identifier assignment mode" << "\n";
                throw ParsingException(error.str());
            }

            line = 0;
            element = block->ToElement();
            color=defaultColor;
            attr = element->Attribute("color");
            color.set(attr);
#ifdef DEBUG_CONF_PARSING
            if (attr) {
                OUTPUT << "line color :" << color << endl;
            }
#endif
            attr = element->Attribute("line");
            if (attr) {
                line = atoi(attr);
            }
            attr = element->Attribute("plane");
            if (attr) {
                plane = atoi(attr);
            }
            attr = element->Attribute("values");
            if (attr) {
                string str(attr);
                position.pt[0] = 0;
                position.pt[1] = line;
                position.pt[2] = plane;
                int n = str.length();
                for(int i=0; i<n; i++) {
                    if (str[i]=='1') {
                        position.pt[0]=i;
                        loadBlock(element, ids == ORDERED ? ++indexBlock:IDPool[indexBlock++],
                                  bcb, position, color, false);
                    }
                }
            }
            block = block->NextSibling("blocksLine");
        } // end while (blocksLine)

        // Reading block boxes
        block = xmlBlockListNode->FirstChild("blockBox");
        while (block) {
            if (ids == MANUAL) {
                stringstream error;
                error << "blocksLine element cannot be used in MANUAL identifier assignment mode" << "\n";
                throw ParsingException(error.str());
            }

            element = block->ToElement();
            color=defaultColor;
            attr = element->Attribute("color");
            color.set(attr);
#ifdef DEBUG_CONF_PARSING
            if (attr) {
                OUTPUT << "box color :" << color << endl;
            }
#endif
            Vector3D boxOrigin(0,0,0);
            attr = element->Attribute("boxOrigin");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                boxOrigin.pt[0] = stof(str.substr(0,pos1));
                boxOrigin.pt[1] = stof(str.substr(pos1+1,pos2-pos1-1));
                boxOrigin.pt[2] = stof(str.substr(pos2+1,str.length()-pos1-1));
#ifdef DEBUG_CONF_PARSING
                OUTPUT << "new boxOrigine:" << boxOrigin << endl;
#endif
            }

            Vector3D boxDest(world->lattice->gridSize[0]*world->lattice->gridScale[0],
                             world->lattice->gridSize[1]*world->lattice->gridScale[1],
                             world->lattice->gridSize[2]*world->lattice->gridScale[2]);
            attr = element->Attribute("boxSize");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                boxDest.pt[0] = boxOrigin.pt[0] + stof(str.substr(0,pos1));
                boxDest.pt[1] = boxOrigin.pt[1] + stof(str.substr(pos1+1,pos2-pos1-1));
                boxDest.pt[2] = boxOrigin.pt[2] + stof(str.substr(pos2+1,str.length()-pos1-1));
#ifdef DEBUG_CONF_PARSING
                OUTPUT << "new boxDest:" << boxDest << endl;
#endif
            }

            assert(world->lattice!=nullptr);

            Vector3D pos;
            for (short iz=0; iz<world->lattice->gridSize[2]; iz++) {
                for (short iy=0; iy<world->lattice->gridSize[1]; iy++) {
                    for (short ix=0; ix<world->lattice->gridSize[0]; ix++) {
                        position.set(ix,iy,iz);
                        pos = world->lattice->gridToWorldPosition(position);

                        if (pos.isInBox(boxOrigin,boxDest)) {
                            if (position == Cell3DPosition(1,0,0)) {
                                cout << "isInBox: " << pos.isInBox(boxOrigin,boxDest) << endl;
                            }

                            loadBlock(element,
                                      ids == ORDERED ? ++indexBlock : IDPool[indexBlock++],
                                      bcb, position, color, false);
                        }
                    }
                }
            }

            block = block->NextSibling("blockBox");
        } // end while (blockBox)*/

        // Reading CSG Object
        block = xmlBlockListNode->FirstChild("csg");
        if (block) {
            TiXmlElement *element = block->ToElement();
            string str = element->Attribute("content");

            Vector3D translate = Vector3D(0,0,0);
            attr = element->Attribute("translate");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                translate.pt[0] = stof(str.substr(0,pos1));
                translate.pt[1] = stof(str.substr(pos1+1,pos2-pos1-1));
                translate.pt[2] = stof(str.substr(pos2+1,str.length()-pos1-1));

#ifdef DEBUG_CONF_PARSING
                OUTPUT << "csg translate :" << translate << endl;
#endif
            }

            BoundingBox bb;
            bool boundingBox = true;
            element->QueryBoolAttribute("boundingBox", &boundingBox);
            bool offsetBoundingBox = true;
            element->QueryBoolAttribute("offset", &offsetBoundingBox);

            CSGParser parser;
            CSGNode *csgRoot = parser.parseCSG(str);
            csgRoot->toString();

            if (boundingBox) csgRoot->boundingBox(bb);

            Vector3D csgPos;
            for (short iz = 0; iz <= world->lattice->getGridUpperBounds()[2]; iz++) {
                const Cell3DPosition& glb = world->lattice->getGridLowerBounds(iz);
                const Cell3DPosition& ulb = world->lattice->getGridUpperBounds(iz);
                for (short iy = glb[1]; iy <= ulb[1]; iy++) {
                    for (short ix = glb[0]; ix <= ulb[0]; ix++) {
                        position.set(ix,iy,iz);
                        csgPos = world->lattice->gridToUnscaledWorldPosition(position);

                        if (offsetBoundingBox) {
                            csgPos.pt[0] += bb.P0[0] - 1.0;
                            csgPos.pt[1] += bb.P0[1] - 1.0;
                            csgPos.pt[2] += bb.P0[2] - 1.0;
                        } else {
                            csgPos.pt[0] += bb.P0[0];
                            csgPos.pt[1] += bb.P0[1];
                            csgPos.pt[2] += bb.P0[2];
                        }

                        if (world->lattice->isInGrid(position)
                            and csgRoot->isInside(csgPos, color)
                            // @note Ignore position already filled through other means
                            // this can be used to initialize some parameters on select
                            // modules using `<block>` elements
                            and not world->lattice->cellHasBlock(position)) {
                            loadBlock(element,
                                      ids == ORDERED ? ++indexBlock : IDPool[indexBlock++],
                                      bcb, position, color, false);
                        }
                    }
                }
            }
        }
    } else { // end if

        cerr << "warning: no Block List in configuration file" << endl;
    }
}

void Simulator::parseTarget() {
    Target::targetListNode = xmlWorldNode->FirstChild("targetList");
    if (Target::targetListNode) {
        // Load initial target (BlockCode::target = nullptr if no target specified)
        BlockCode::target = Target::loadNextTarget();
    }
}

void Simulator::parseObstacles() {
    // loading the obstacles
    TiXmlNode *nodeObstacle = xmlWorldNode->FirstChild("obstacleList");
    if (nodeObstacle) {
        Color defaultColor = DARKGREY;
        TiXmlElement* element = nodeObstacle->ToElement();
        const char *attr= element->Attribute("color");
        defaultColor.set(attr);

        nodeObstacle = nodeObstacle->FirstChild("obstacle");
        Cell3DPosition position;
        Color color;
        while (nodeObstacle) {
            element = nodeObstacle->ToElement();
            color=defaultColor;
            attr = element->Attribute("color");
            color.set(attr);
#ifdef DEBUG_CONF_PARSING
            if (attr) {
                OUTPUT << "color :" << color << endl;
            }
#endif
            attr = element->Attribute("position");
            if (attr) {
                string str(attr);
                int pos1 = str.find_first_of(','),
                    pos2 = str.find_last_of(',');
                position.pt[0] = stoi(str.substr(0,pos1));
                position.pt[1] = stoi(str.substr(pos1+1,pos2-pos1-1));
                position.pt[2] = stoi(str.substr(pos2+1,str.length()-pos1-1));
#ifdef DEBUG_CONF_PARSING
                OUTPUT << "position : " << position << endl;
#endif
            }

            world->addObstacle(position, color);
            nodeObstacle = nodeObstacle->NextSibling("obstacle");
        } // end while (nodeObstacle)
    }
}

void Simulator::parseCustomizations() {
    TiXmlNode *customizationNode = xmlWorldNode->FirstChild("customization");
    if (customizationNode) {
        TiXmlNode *rotationDelayNode = customizationNode->FirstChild("motionDelay");

        if (rotationDelayNode) {
            TiXmlElement* element = rotationDelayNode->ToElement();
            const char *attr= element->Attribute("multiplier");

            if (attr != nullptr) {
                motionDelayMultiplier = atof(attr);
            }
        }
    }
}

void Simulator::startSimulation() {
    // Connect all blocks – TODO: Check if needed to do it here (maybe all blocks are linked on addition)
    world->linkBlocks();

    // Finalize scheduler configuration and start simulation if autoStart is enabled
    Scheduler *scheduler = getScheduler();
    //scheduler->sem_schedulerStart->post();
    scheduler->setState(Scheduler::NOTSTARTED);
    if (scheduler->willAutoStart())
        scheduler->start(scheduler->getSchedulerMode());

    // Start replay export if enabled
    if (cmdLine.isReplayEnabled()) {
        auto replay = ReplayExporter::getInstance();
        ReplayExporter::enable(true);
        replay->writeHeader();
        // ReplayExporter::getInstance()->exportKeyframe();
    }

    // Enter graphical main loop
    GlutContext::mainLoop();
}

ruint Simulator::getRandomUint() {
    return generator();
}

} // Simulator namespace
