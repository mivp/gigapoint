#include "Utils.h"
#include "NodeGeometry.h"

#include <iostream>
#include <fstream>
#include <assert.h>
#include <float.h>
#include <bitset>
#include <map>
#include <math.h>
#include <sstream>

using namespace std;
#ifndef STANDALONE_APP
using namespace omega;
#endif

namespace gigapoint {

NodeGeometry::NodeGeometry(string _name): index(-1), numpoints(0), level(-1), parent(NULL),updateCache(NULL),
										  hierachyloaded(false), loadstate(STATE_NONE), initvbo(false), haschildren(false),
                                          vertexbuffer(-1), colorbuffer(-1), dirty(false),updating(false),datafile("unset")
                                          {
	name = _name;
	//tightbbox[0] = tightbbox[1] = tightbbox[2] = FLT_MAX;
	//tightbbox[3] = tightbbox[4] = tightbbox[5] = FLT_MIN;
	for(int i=0; i < 8; i++)
		children[i] = NULL;
	level = name.length() - 1;
	for(int i=0; i < 6; i++)
		bbox[i] = -1;
}

NodeGeometry::~NodeGeometry() {
    freeData();
}

void NodeGeometry::addPoint(float x, float y, float z) {
	vertices.push_back(x); 
	vertices.push_back(y);
	vertices.push_back(z);
	
	//numpoints++;
	//tightbbox[0] = min(tightbbox[0], x); tightbbox[1] = min(tightbbox[1], y); tightbbox[2] = min(tightbbox[2], z);
	//tightbbox[3] = max(tightbbox[3], x); tightbbox[4] = max(tightbbox[4], y); tightbbox[5] = max(tightbbox[5], z);
}

void NodeGeometry::addColor(float r, float g, float b) {
	colors.push_back(r); 
	colors.push_back(g);
	colors.push_back(b);
}

void NodeGeometry::setBBox(const float* bb) {
	for(int i=0; i < 6; i++)
		bbox[i] = bb[i];
}

void NodeGeometry::setTightBBox(const float* bb) {
	for(int i=0; i < 6; i++)
		tightbbox[i] = bb[i];
	spherecentre[0] = (tightbbox[0] + tightbbox[3])*0.5;
	spherecentre[1] = (tightbbox[1] + tightbbox[4])*0.5;
	spherecentre[2] = (tightbbox[2] + tightbbox[5])*0.5;
	float bmin[3] = { tightbbox[0], tightbbox[1], tightbbox[2] };
	sphereradius = Utils::distance(spherecentre, bmin);
}

string NodeGeometry::getHierarchyPath() {
	assert(info);
	string path = "r/";
	int numparts = level / info->hierarchyStepSize;
	for(int i=0; i < numparts; i++) {
		path += name.substr(1+i*info->hierarchyStepSize, info->hierarchyStepSize) + "/";
	}
	
	return path;
}

int NodeGeometry::loadHierachy(LRUCache* lrucache, bool force) {
    
    if (!canLoadHierarchy())
		return 0;

	if(hierachyloaded && !force)
        return 0;

    hrc_filename = info->dataDir + info->octreeDir + "/" + getHierarchyPath() + name + ".hrc";

	assert(info);

    cout << "Load hierachy file: " << hrc_filename << endl;   
    NodeGeometry* n = this;

	if(level == 0) { // root
		setBBox(info->boundingBox);
		setTightBBox(info->tightBoundingBox);
	} 

	list<HRC_Item> stack;
	list<HRC_Item> decoded;

	FILE *f;long len; unsigned char *data;
	f=fopen(hrc_filename.c_str(),"rb");
	if(f == NULL){
		std::cout << "Cannot find " << hrc_filename << "!!!" << std::endl;
		return -1;
	}
	fseek(f,0,SEEK_END);len=ftell(f);fseek(f,0,SEEK_SET);
	data= new unsigned char[len+1];fread(data,1,len,f);fclose(f);

	// root of subtree
	int offset = 0;
	unsigned char children = data[offset];

    n->numpoints = (data[offset+4] << 24) | (data[offset+3] << 16) | (data[offset+2] << 8) | data[offset+1]; // little andian
	offset += 5;

	std::bitset<8> x(children);
    //cout << "Root children: " << x << endl;
    //cout << "Root numpoints: " << n->numpoints << endl;

    stack.push_back(HRC_Item(name, children, n->numpoints));
    //cout << "Root name: " << name << endl;
	while(stack.size() > 0){

		HRC_Item snode = stack.front();
		stack.pop_front();

		unsigned char mask = 1;
		for (int i=0; i < 8; i++) {
			if( (int)(snode.children & mask) != 0) {
				stringstream ss;
				ss << i;
				string childname = snode.name + ss.str();
				unsigned char child_children = data[offset];
				unsigned int child_numpoints = (data[offset+4] << 24) | (data[offset+3] << 16) | (data[offset+2] << 8) | data[offset+1];
				
				stack.push_back(HRC_Item(childname, child_children, child_numpoints));
				decoded.push_back(HRC_Item(childname, child_children, child_numpoints));

				offset += 5;
			}
			mask = mask * 2;
		}

		if(offset == len)
			break;
	}
    
    map<string, NodeGeometry*> nodes;

    if( !lrucache->contains(name) ) {
        nodes[name] = this;
        //cout << "added " << name << " to global nodes " << endl;
    }

	for(list<HRC_Item>::iterator it = decoded.begin(); it != decoded.end(); it++) {
		HRC_Item item = *it;
		std::bitset<8> children_bit(item.children);
		//cout << "Node: " << item.name << " children: " << children_bit << " numpoints: " << item.numpoints << endl;

		string str_ind = item.name.substr(item.name.length()-1, 1);
		//cout << "index str: " << str_ind << endl;

		string parentname = item.name.substr(0, item.name.length()-1);
        NodeGeometry* pnode = nodes[parentname];
		assert(pnode);

        //TODO check if childnode already exists or if its new!
        NodeGeometry* cnode = NULL;
        bool exist = lrucache->tryGet(item.name, cnode);
        if(!exist) {
            if (dirty)
                cout << "creating new node " << item.name << endl;
            cnode = new NodeGeometry(item.name);
            assert(cnode);
            int cindex = atoi(str_ind.c_str());
            cnode->setLevel(item.name.length()-1);
            cnode->setIndex(cindex); // TODO is this needed ?
            cnode->setNumPoints(item.numpoints);
            cnode->setHasChildren(item.children > 0);
            float cbbox[6], tightcbbox[6];
            Utils::createChildAABB(pnode->getBBox(), cindex, cbbox);
            Utils::createChildAABB(pnode->getTightBBox(), cindex, tightcbbox);
            cnode->setBBox(cbbox);
            cnode->setInfo(pnode->getInfo());
            cnode->setTightBBox(tightcbbox);
            //cnode->printInfo();
            pnode->addChild(cnode);
            pnode->setHasChildren(true);
            cnode->loadHierachy(lrucache);
            nodes[item.name] = cnode;
        }
    }

	hierachyloaded = true;    
	return 0;
}


ifstream::pos_type NodeGeometry::getFilesize(const char* filename)
{
        ifstream in(filename, ifstream::ate | ifstream::binary);
        return in.tellg();
}

int NodeGeometry::loadData() {

    if(isLoaded())
        return 0;
	
	assert(info);

    loadstate = STATE_LOADING;

	string filename = info->dataDir + info->octreeDir + "/" + getHierarchyPath() + name + ".bin";
    // cout << "Load file: " << filename << endl;
	datafile = filename;
    //cout << "start reading " << datafile <<  std::endl;

	ifstream reader;
	reader.open (filename.c_str(), ifstream::in | ifstream::binary);
	
	while(reader.good()) {
		char* buffer = new char[info->pointByteSize];
		reader.read(buffer, info->pointByteSize);

		if(!reader.good()){
			if(buffer)
            	delete [] buffer;
			break;
		}

		int offset = 0;
		for(int i = 0; i < info->pointAttributes.size(); i++){
			int attribute = info->pointAttributes[i];
			
			if(attribute == POSITION_CARTESIAN){
				int* iBuffer = reinterpret_cast<int*>(buffer+offset);
				float x, y, z;
				x = (iBuffer[0] * info->scale) + bbox[0];
				y = (iBuffer[1] * info->scale) + bbox[1];
				z = (iBuffer[2] * info->scale) + bbox[2];
			
				offset += 3 * sizeof(float);
				addPoint(x, y, z);
				//cout << "pos: " << x << " " << y << " " << z << endl;
				//cout << "bbox: " << bbox[0] << " " << bbox[1] << " " << bbox[2] <<endl;

			}else if(attribute == INTENSITY) {
				//unsigned short* usBuffer = reinterpret_cast<unsigned short*>(buffer+offset);
				//point.intensity = usBuffer[0];
				offset += 2;

			}else if(attribute == CLASSIFICATION ) {
				//unsigned char* ucBuffer = reinterpret_cast<unsigned char*>(buffer+offset);
				//point.classification = ucBuffer[0];
				offset += 1;

			}else if(attribute == COLOR_PACKED){
				unsigned char* ucBuffer = reinterpret_cast<unsigned char*>(buffer+offset);
				//float r = ucBuffer[0]/255.0;
				//float g = ucBuffer[1]/255.0;
				//float b = ucBuffer[2]/255.0;
				offset += 4 * sizeof(char);
				//addColor(r, g, b);
				addColor(ucBuffer[0], ucBuffer[1], ucBuffer[2]);
				//cout << "colors: " << r << " " << g << " " << b << endl;

			}else {
				cout << "Error: Invalid attribute!" << endl;
			}
		}
		
		if(buffer)
			delete [] buffer;
	}

	reader.close();
    //cout << "done reading " << filename.c_str() << std::endl;
    
    loadstate = vertices.size() > 0 ? STATE_LOADED : STATE_NONE;
  
    return 0;
}

void NodeGeometry::printInfo() {
	cout << endl << "Node: " << name << " level: " << level << " index: " << index << endl;
    cout << "# points: " << numpoints << " loaded " << isLoaded() << endl;
	cout << "data file: " << datafile << endl;
    cout << "children: ";
    for(int i=0; i < 8; i++)
        cout << (children[i] != NULL);
    cout << endl;\
	cout << "bbox: ";
	for(int i=0; i < 6; i++)
		cout << bbox[i] << " ";
	cout << endl;

	/*
	cout << "tight bbox: ";
	for(int i=0; i < 6; i++)
		cout << tightbbox[i] << " ";
	cout << endl;
	*/

	if(!isLoaded())
		return;

	cout << "first 5 points: " << endl;
	for(int i=0; i < min(numpoints, 5); i++) {
		cout << vertices[i*3+0] << " " << vertices[i*3+1] << " " << vertices[i*3+2] << "   ";
		if(colors.size() > 0) 
			cout << colors[i*3+0] << " " << colors[i*3+1] << " " << colors[i*3+2];
		cout << endl;
	}

	if(initvbo)
		cout << "vertexbuffer: " << vertexbuffer << " colorbuffer: " << colorbuffer << endl;
    cout << "dirty: " << dirty << endl;
    cout << "updatecache: " << (updateCache!=NULL) << endl;
}

int NodeGeometry::initVBO() {
    if(initvbo) {
        std::cout << "reinitializing VBO of Node " << name << std::endl;
        glDeleteBuffers(1, &vertexbuffer);
        glDeleteBuffers(1, &colorbuffer);
        initvbo = false;
    }
	glGenBuffers(1, &vertexbuffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
	glBufferData(GL_ARRAY_BUFFER, vertices.size()*sizeof(float), &vertices[0], GL_STATIC_DRAW);

	glGenBuffers(1, &colorbuffer);
	glBindBuffer(GL_ARRAY_BUFFER, colorbuffer);
	glBufferData(GL_ARRAY_BUFFER, colors.size()*sizeof(unsigned char), &colors[0], GL_STATIC_DRAW);

    initvbo = true;

    return 0;
}
    
void NodeGeometry::getRangeInfo(const Option* option, float &range_min, float &range_max, float &range) {
    if(option->elevationDirection == 0) {
        range = info->tightBoundingBox[3] - info->tightBoundingBox[0];
        range_min = info->tightBoundingBox[0] + option->elevationRange[0] * range;
        range_max = info->tightBoundingBox[0] + option->elevationRange[1] * range;
    }
    else if (option->elevationDirection == 1) {
        range = info->tightBoundingBox[4] - info->tightBoundingBox[1];
        range_min = info->tightBoundingBox[1] + option->elevationRange[0] * range;
        range_max = info->tightBoundingBox[1] + option->elevationRange[1] * range;
    }
    else {
        range = info->tightBoundingBox[5] - info->tightBoundingBox[2];
        range_min = info->tightBoundingBox[2] + option->elevationRange[0] * range;
        range_max = info->tightBoundingBox[2] + option->elevationRange[1] * range;
    }
}

#ifdef STANDALONE_APP
void NodeGeometry::draw(const float MV[16], const float MVP[16], Material* material, const int height) {
#else
void NodeGeometry::draw(Material* material, const int height) {
#endif
    
	if(isLoading() || !isLoaded())
		return;
    
	if(!initvbo)
		initVBO();
	
    Shader* shader = material->getShader();
	Option* option = material->getOption();
	ColorTexture* texture = ((MaterialPoint*)material)->getColorTexture();
	shader->bind();
	texture->bind();
#ifndef STANDALONE_APP
	if(oglError) return;
#endif
	
    unsigned int attribute_vertex_pos = shader->attribute("VertexPosition");
    //cout << "Vertex Position: " << attribute_vertex_pos << endl;
    glEnableVertexAttribArray(attribute_vertex_pos);  // Vertex position
    glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
    glVertexAttribPointer(
        attribute_vertex_pos, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        0,                 // no extra data between each position
        0                  // offset of first element
    );
#ifndef STANDALONE_APP
    if(oglError) return;
#endif

    unsigned int attribute_color_pos;
    if(option->material == MATERIAL_RGB) {
    attribute_color_pos = shader->attribute("VertexColor");
    //cout << "Vertex Color: " << attribute_color_pos << endl;   
    glEnableVertexAttribArray(attribute_color_pos);  // Vertex position
    glBindBuffer(GL_ARRAY_BUFFER, colorbuffer);
    glVertexAttribPointer(
        attribute_color_pos, // attribute
        3,                 // number of elements per vertex, here (r, g, b)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_FALSE,          // take our values as-is
        0,                 // no extra data between each position
        0                  // offset of first element
    );
#ifndef STANDALONE_APP
    if(oglError) return;
#endif
    }
	
	shader->transmitUniform("uColorTexture", (int)0);
    float range, range_min, range_max;
    getRangeInfo(option, range_min, range_max, range);
    shader->transmitUniform("uElevationDirection", option->elevationDirection);
	shader->transmitUniform("uHeightMinMax", range_min, range_max);
	shader->transmitUniform("uScreenHeight", (float)height);
    shader->transmitUniform("uPointScale", (float)option->pointScale[0]);
    shader->transmitUniform("uPointSizeRange", (float)option->pointSizeRange[0], (float)option->pointSizeRange[1]);
#ifdef STANDALONE_APP
    shader->transmitUniform("uMV", MV);
    shader->transmitUniform("uMVP", MVP);
#endif

	glDrawArrays(GL_POINTS, 0, vertices.size()/3);
#ifndef STANDALONE_APP
	if(oglError) return;
#endif
	   
    glDisableVertexAttribArray(attribute_vertex_pos);
    if(option->material == MATERIAL_RGB)
    	glDisableVertexAttribArray(attribute_color_pos);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    shader->unbind();
    texture->unbind();
}

void NodeGeometry::freeData(bool keepupdatecache) {
	//cout << "Free data for node: " << name << endl;
	if(initvbo) {
		glDeleteBuffers(1, &vertexbuffer);
		glDeleteBuffers(1, &colorbuffer);
		initvbo = false;
	}
	if(isLoaded()) {
		vertices.clear();
		colors.clear();
        if (!keepupdatecache) // to prevent this node from landing in the loadingqueue  again
            loadstate = STATE_NONE;
	}
    if (keepupdatecache)
        return;
    if (updateCache != NULL)
    {
        if ( isLoading() || isUpdating()) {
            cout << "node is freed while loading or updating" << std::endl;
        }
        updateCache->freeData();
        delete updateCache;
        updateCache=NULL;
    }
}

void NodeGeometry::Update() {

    if (!dirty || updateCache == NULL)
        return;

    if (!updateCache->isLoaded())
        return;

    freeData(true); //keep updateCache=true

    //update data
    vertices=updateCache->vertices; //slow copy reference not values
    colors=updateCache->colors;
    for(int i=0; i < 8; i++) {
        if ( (children[i] == NULL) && (updateCache->children[i] != NULL) ) {
            cout << "node has new child" << name << " " << updateCache->children[i]->name << endl;
            children[i]=updateCache->children[i];
        }
    }

    //setBBox(updateCache->getBBox());
    cout << "updated " << name <<
            " numPoins: old/new " << numpoints << " " << updateCache->numpoints << endl;    

    haschildren=updateCache->hasChildren();
    numpoints=updateCache->numpoints;

    //delete updateCache
    updateCache->freeData();
    delete updateCache;
    updateCache = NULL;
    dirty = false;
    updating=false;
    hierachyloaded = false;

}

void NodeGeometry::initUpdateCache()
{
    updating=true;
    updateCache = new NodeGeometry(name);
    updateCache->setInfo(info);
    updateCache->setBBox(getBBox());

    updateCache->setIndex(index);
    updateCache->setLevel(level);
    updateCache->setNumPoints(numpoints);

}

std::vector< Point > NodeGeometry::getPointsInSphericalNeighbourhood(Point current, float search_r){
    std::vector< Point > points;
    std::vector<NodeGeometry *> nodesInRange;
    nodesInRange.push_back(this);
    NodeGeometry *node;
    NodeGeometry *c=NULL;
    float pos[3];
    float dist;

    while(!nodesInRange.empty())
    {
        node=nodesInRange.back();
        nodesInRange.pop_back();

        int numpoints = node->vertices.size() / 3;
        for(int i=0; i < numpoints; i++) {
            pos[0] = node->vertices[3*i];
            pos[1] = node->vertices[3*i+1];
            pos[2] = node->vertices[3*i+2];
            if (DIST3(pos,current.position) < search_r)
            {
                Point p(this,i);
                p.color[0]=node->colors[3*i];
                p.color[1]=node->colors[3*i+1];
                p.color[2]=node->colors[3*i+2];
                p.position[0]=node->vertices[3*i];
                p.position[1]=node->vertices[3*i+1];
                p.position[2]=node->vertices[3*i+2];
                //p.index.nodename=node->name;
                points.push_back(p);
            }
        }


        // stop at certain treedepth. treenodes might not be present on other cave nodes
        if (node->level > MIN_TREE_DEPTH)
            continue;

        //check all children if they are close enough
        for (int i=0;i<8;++i)
        {
            c=node->getChild(i);
            if (c!=NULL)
            {
                float dist=DIST3(node->getSphereCentre(),c->getSphereCentre());
                dist=dist - node->getSphereRadius()- c->getSphereRadius();
                if (dist <0 || dist < search_r)
                    nodesInRange.push_back(c);
            }
        }
    }
    return points;
}



void NodeGeometry::getPointData(Point &point)
{
    int idx = point.index.index;
    point.position[0] = vertices[0+idx*3];
    point.position[1] = vertices[1+idx*3];
    point.position[2] = vertices[2+idx*3];
    point.color[0]    = colors[0+idx*3];
    point.color[1]    = colors[1+idx*3];
    point.color[2]    = colors[2+idx*3];
}

void NodeGeometry::setPointColor(Point &point, int r, int g, int b)
{
    colors[3*point.index.index+0]=(unsigned char)r;
    colors[3*point.index.index+1]=(unsigned char)g;
    colors[3*point.index.index+2]=(unsigned char)b;
}


#ifndef STANDALONE_APP
// interaction
void NodeGeometry::findHitPoint(const omega::Ray& r, HitPoint* point) {
	// check with the whole node first
	Vector3f pos = Vector3f(spherecentre[0], spherecentre[1], spherecentre[2]);
	std::pair<bool, omega::real> result = r.intersects(Sphere(pos, sphereradius));
	if(!result.first)
		return;

	// check all points
	int numpoints = vertices.size() / 3;
	for(int i=0; i < numpoints; i++) {
		pos = Vector3f(vertices[3*i], vertices[3*i+1], vertices[3*i+2]);
		result = r.intersects(Sphere(pos, 2));
		if(result.first) {
			float dis = (float)result.second;
			if (point->distance == -1 || point->distance > dis) {
				point->distance = dis;
				point->position[0] = pos[0];
				point->position[1] = pos[1];
				point->position[2] = pos[2];
                //std::cout << "hitpoint index is " << i <<std::endl;du an
			}
		}
    }
}
#endif


}; //namespace gigapoint


