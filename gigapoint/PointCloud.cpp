#include "PointCloud.h"
#include "Utils.h"


#include <iostream>

using namespace std;
using namespace omicron;

namespace gigapoint {


PointCloud::PointCloud(Option* opt, bool mas): option(opt), master(mas), pauseUpdate(false),fullReload(false),
                                               width(0), height(0), frameBuffer(0),
                                               lrucache(NULL),_unload(false),render(true),
                                               materialPoint(0), materialEdl(0),quadVao(0), quadVbo(0),
                                               needReloadShader(false), printInfo(false) {
}

PointCloud::~PointCloud() {
    // destroy tree
	if(pcinfo)
		delete pcinfo;      
	if(materialPoint)
		delete materialPoint;
	if(materialEdl)
		delete materialEdl;
	if(frameBuffer)
		delete frameBuffer;
	if (glIsBuffer(quadVbo))
		glDeleteBuffers(1, &quadVbo);
	if (glIsVertexArray(quadVao))
		glDeleteVertexArrays(1, &quadVao);
}

void PointCloud::initMaterials() {
	if(!materialPoint)
		materialPoint = new MaterialPoint(option);
	if(!materialEdl)
		materialEdl = new MaterialEdl(option);
	if(!frameBuffer) {
		vector<string> targets;
		targets.push_back("tex0");
		frameBuffer = new FrameBuffer(targets, width, height, true);
		frameBuffer->init();
		if(oglError) return;
	}
}

int PointCloud::initPointCloud() {

	// Option
	if(!option){
		cout << "Error: cannot load option " << endl;
		return -1;
	}
	if(master)
		Utils::printOption(option);

	// PC Info
	pcinfo = Utils::loadPCInfo(option->dataDir);
	if(!pcinfo) {
		cout << "Error: cannot load pc info" << endl;
		return -1;
	}
	if(master)
		Utils::printPCInfo(pcinfo);

    if (!lrucache)
        lrucache = new LRUCache(option->maxNodeInMem);

    // root node
	string name = "r";
	root = new NodeGeometry(name);
	root->setInfo(pcinfo);    
    if(root->loadHierachy(lrucache)) {
		cout << "fail to load root hierachy" << endl;
		return -1;
	}
	if(root->loadData()) {
		cout << "fail to load root data " << endl;
		return -1;
	}

    //preDisplayListSize = 0;

	numLoaderThread = option->numReadThread;
	// reading threads
	if(nodeLoaderThreads.size() == 0) {
    	for(int i = 0; i < numLoaderThread; i++) {
    		NodeLoaderThread* t = new NodeLoaderThread(nodeQueue, option->maxLoadSize);
    		t->start();
    		nodeLoaderThreads.push_back(t);
	    }
	
    }


    preloadUpToLevel(option->preloadToLevel);

	return 1;
}



int PointCloud::preloadUpToLevel(const int level) {
	priority_queue<NodeWeight> priority_queue;
	priority_queue.push(NodeWeight(root, 1));

	unsigned numloaded = 0;

	cout << "Preload data to tree level " << level << " ..." << endl;

	while(priority_queue.size() > 0) {

		NodeGeometry* node = priority_queue.top().node;
    	priority_queue.pop();
		
		bool canload = false;
		if(numloaded + node->getNumPoints() < option->visiblePointTarget)
    		canload = true;

    	if(!canload)
    		continue;

        node->loadHierachy(lrucache);
		node->loadData();
		lrucache->insert(node->getName(), node);

		if(node->getLevel() >= level)
			continue;

		for(int i=0; i < 8; i++) {
			if(node->getChild(i) == NULL)
				continue;
			priority_queue.push(NodeWeight(node->getChild(i), 1.0/node->getChild(i)->getLevel()));
		}

	}

	return 0;
}


int PointCloud::updateVisibility(const float MVP[16], const float campos[3], const int width, const int height) {
    if (pauseUpdate)
        return 0;

	this->width = width;
	this->height = height;

	float V[6][4];
    Utils::getFrustum(V, MVP);
	    
    displayList.clear();
    numVisibleNodes = 0;
    numVisiblePoints = 0;

    unsigned int start_time = Utils::getTime();
    if (!root)
        return 0;
    root->loadHierachy(lrucache);

#ifdef ONLINEUPDATE
    if (option->onlineUpdate) {
        //Utils::updatePCInfo(option->dataDir,root->getInfo());

        //root->checkForUpdate();
        root->Update();
    }
#endif

    priority_queue<NodeWeight> priority_queue;
    priority_queue.push(NodeWeight(root, 1));

    while(priority_queue.size() > 0){
    	NodeGeometry* node = priority_queue.top().node;
    	priority_queue.pop();
    	bool visible = false;

#ifdef ONLINEUPDATE
        if (option->onlineUpdate)
            node->Update();
#endif

    	if(Utils::testFrustum(V, node->getBBox()) >= 0 && numVisiblePoints + node->getNumPoints() < option->visiblePointTarget)
    		visible = true;
	    
	    if(!visible)
	    	continue; 

	    numVisibleNodes++;
		numVisiblePoints += node->getNumPoints();

        node->loadHierachy(lrucache);

        if(!node->inQueue() && node->canAddToQueue() ) {
			node->setInQueue(true);
            //cout << "adding " << node->getName() << " to queue" << niq << ncaq << endl;
			nodeQueue.add(node);
		}
        else if (node->isDirty() && !node->isUpdating() ) {
            node->setInQueue(true);
            //cout << "adding " << node->getName() << " to queue because its dirty" << endl;
            nodeQueue.add(node);
        }		
		displayList.push_back(node);
		lrucache->insert(node->getName(), node);

		if(Utils::getTime() - start_time > 150)
			return 0;
		
		// add children to priority_queue
		for(int i=0; i < 8; i++) {
			if(node->getChild(i) == NULL)
				continue;
			//calculte weight
			float* centre = node->getSphereCentre();
			float radius = node->getSphereRadius();
			float distance = Utils::distance(centre, campos);
			float fov = 0.6;
			float pr = 1 / tan(fov) * radius / sqrt(distance*distance - radius*radius);
			float weight = pr;
			if(distance - radius < 0)
				weight = FLT_MAX;

			float screenpixelradius = height * pr;
			if(screenpixelradius < option->minNodePixelSize)
				continue;

			priority_queue.push(NodeWeight(node->getChild(i), weight));
			//priority_queue.push(NodeWeight(node->getChild(i), 1.0/node->getChild(i)->getLevel()));
		}

    }

    return 0;
}

// TODO retest if it still works
void PointCloud::unload() {
    cout << "unloading everything" << endl;
    lrucache->clear();
    //unload all the nodes    
    //nodes->erase(nodes->begin(),nodes->end());
    root = NULL;
    displayList.clear();
    _unload=false;
}

void PointCloud::resetRootHierarchy() {
    root->loadHierachy(lrucache,true);
}

bool PointCloud::flagNodeAsDirty(const std::string &nodename)
{
    NodeGeometry* node=NULL;
    if (lrucache->tryGet(nodename,node))
    {
        node->setDirty();
    } else {  // we are updating a node that doesn't exist yet in the current tree.
        //we have to find the next hierarchy node and mark that as dirty
        bool foundHierarchyNode = false;
        string parentname = nodename;
        while (!foundHierarchyNode) {
             parentname = parentname.substr(0,parentname.size()-1);
             lrucache->tryGet(parentname,node);
             if (node->canLoadHierarchy())
             {
                 foundHierarchyNode=true;
                 node->loadHierachy(lrucache,true);
             }
        }
    }
    return true;
}

void PointCloud::reload() {
    render = false;
    pauseUpdate=true;
    //empty loading queue
    if (0!=nodeQueue.size())
    {
        cout << "waiting for loading queue to empty" << endl;
        return;
    }
    //empty lru
    lrucache->clear();
    //unload all the nodes
    //nodes->erase(nodes->begin(),nodes->end());
    displayList.clear();
    //redo init
    initPointCloud();
    needReloadShader = true;
    pauseUpdate=false;
    fullReload = false;
    render = true;
}

void PointCloud::debug() {
    Utils::printPCInfo(root->getInfo());
    cout << "numVisibleNodes: " << numVisibleNodes << " numVisiblePoints: " << numVisiblePoints <<
            " nodeQueue size: " << nodeQueue.size() << " lrucache size: " << lrucache->size() << endl;

    /*
    for(map<string, NodeGeometry*>::iterator it = nodes->begin(); it != nodes->end(); it++) {
        NodeGeometry* node=(*it).second;
        //cout << "Node: " << (*it).first << endl;
        node->printInfo();
    }
    */
    printInfo = false;
}

void PointCloud::draw() {

    if (_unload)
        unload();

    if (fullReload)
        reload();

	initMaterials();


	if(needReloadShader) {
		materialPoint->reloadShader(); 
		needReloadShader = false;
		if(oglError) return;
	}
	if(printInfo) {
        debug();
	}

    if (!render)
        return;

	//draw to bufferframe
	if(option->filter != FILTER_NONE) {
		frameBuffer->bind();
		frameBuffer->clear();
	}


	for(list<NodeGeometry*>::iterator it = displayList.begin(); it != displayList.end(); it++) {
		NodeGeometry* node = *it;
		node->draw(materialPoint, height);
	}

	if(option->filter != FILTER_NONE) {

		frameBuffer->unbind();
		Shader* edlShader = materialEdl->getShader();
		edlShader->bind();
		edlShader->transmitUniform("uColorTexture", (int)0);
		edlShader->transmitUniform("uScreenWidth", (float)width);
		edlShader->transmitUniform("uScreenHeight", (float)height);
		edlShader->transmitUniform("uEdlStrength", option->filterEdl[0]);
		edlShader->transmitUniform("uRadius", option->filterEdl[1]);
		edlShader->transmitUniform("uOpacity", 1.0f);
		edlShader->transmitUniform2fv("uNeighbours", ((MaterialEdl*)materialEdl)->getNeighbours());

		frameBuffer->getTexture("tex0")->bind();
		
		drawViewQuad();

		frameBuffer->getTexture("tex0")->unbind();
		materialEdl->getShader()->unbind();
    }

}

void PointCloud::drawViewQuad()
{
	if (!quadVbo)
	{
		float points[] = {
			-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
			-1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
			1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f};

		glGenBuffers(1, &quadVbo);

		glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float)*24, points, GL_STATIC_DRAW);
	}

	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	Shader* shader = materialEdl->getShader();
	unsigned int attribute_vertex_pos = shader->attribute("VertexPosition");
	glEnableVertexAttribArray(attribute_vertex_pos);
	glVertexAttribPointer(attribute_vertex_pos, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (const GLvoid*)0);

	unsigned int attribute_vertex_tex_coor = shader->attribute("VertexTexCoord");
	glEnableVertexAttribArray(attribute_vertex_tex_coor);
	glVertexAttribPointer(attribute_vertex_tex_coor, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (const GLvoid*)12);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}



Point PointCloud::getPointFromIndex(const PointIndex_ &index)
{
    Point p(index);
    p.index.node->getPointData(p);
    return p;
}



}; //namespace gigapoint

