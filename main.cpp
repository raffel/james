/* Copyright 2011 Tomas Härdin
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * File:   main.cpp
 * Author: tjoppen
 *
 * Created on February 12, 2010, 3:53 PM
 */
#pragma GCC diagnostic ignored "-Wshadow"

#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/dom/DOMDocument.hpp>
#include <xercesc/dom/DOMElement.hpp>
#include <xercesc/dom/DOMNode.hpp>
#include <xercesc/dom/DOMNodeList.hpp>
#include <xercesc/dom/DOMAttr.hpp>
#include <xercesc/dom/DOMNamedNodeMap.hpp>

#ifndef WIN32
 #include <unistd.h>
#endif

#include "main.h"
#include "libjames/XercesString.h"
#include "Class.h"
#include "BuiltInClasses.h"

using namespace std;
using namespace xercesc;
using namespace james;

static void printUsage() {
    cerr << "USAGE: james [-v] [-d] [-nr] [-nv] [-a] [-cmake targetname] [--dry-run] output-dir list-of-XSL-documents" << endl;
    cerr << " -v\tVerbose mode" << endl;
    cerr << " -d\tGenerate default constructors" << endl;
    cerr << " -nr\tDon't generate constructors taking required elements" << endl;
    cerr << " -nv\tDon't generate constructors taking required elements and vectors" << endl;
    cerr << " -a\tGenerate constructors taking all elements" << endl;
    cerr << " -cmake\tGenerate CMakeLists.txt with all generated .cpp files as part of a library with the specified target name" << endl;
    cerr << " --namespace\tGenerate C++ classes with specified namespace" << endl;
    cerr << " --dry-run\tPerform generation but don't write anything to disk - instead does exit(1) if any file changes" << endl;
    cerr << endl;
    cerr << " Generates C++ classes for marshalling and unmarshalling XML to C++ objects according to the given schemas." << endl;
    cerr << " Files are output in the specified output directory and are named type.h and type.cpp" << endl;
}

//maps namespace abbreviation to their full URIs
map<string, string> nsLUT;

//collection of all generated classes
map<FullName, Class*> classes;

//fake classes which are appended to other classes. see Class::groups and xs::attributeGroup
map<FullName, Class*> groups;

bool verbose = false;
bool generateDefaultCtor = false;
bool generateRequiredCtor = true;
bool generateRequiredAndVectorsCtor = true;
bool generateAllCtor = false;
static std::string cmakeTargetName;
std::string namespacePrefix, np;

static Class* addClass(Class *cl, map<FullName, Class*>& to = classes) {
    if(to.find(cl->name) != to.end()) {
        FullName name = cl->name;
        delete cl;
        throw runtime_error(name.first + ":" + name.second + " defined more than once");
    }

    return to[cl->name] = cl;
}

//set of C++ keywords. initialized by initKeywordSet()
set<string> keywordSet;

//raw list of C++ keywords
const char *keywords[] = {
    "and",
    "and_eq",
    "asm",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "class",
    "compl",
    "const",
    "const_cast",
    "continue",
    "default",
    "delete",
    "do",
    "double",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "not",
    "not_eq",
    "operator",
    "or",
    "or_eq",
    "private",
    "protected",
    "public",
    "register",
    "reinterpret_cast",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_cast",
    "struct",
    "switch",
    "template",
    "this",
    "throw",
    "true",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "wchar_t",
    "while",
    "xor",
    "xor_eq",

    //other "keywords"
    "content",  //complexTypes with simpleContent
};

static void initKeywordSet() {
    //stuff keywords into keywordSet for fast lookup
    for(size_t x = 0; x < sizeof(keywords) / sizeof(const char*); x++)
        keywordSet.insert(keywords[x]);
}

static string fixIdentifier(string str) {
    //strip any bad characters such as dots, colons, semicolons..
    string ret;

    for(size_t x = 0; x < str.size(); x++) {
        char c = str[x];

        if((c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') ||
                c == '_')
            ret += c;
        else
            ret += "_";
    }

    //check if identifier is a reserved C++ keyword, and append an underscore if so
    if(keywordSet.find(ret) != keywordSet.end())
        ret += "_";

    return ret;
}

static string lookupNamespace(string typeName, string defaultNamespace) {
    //figures out namespace URI of given type
    size_t pos = typeName.find_last_of(':');

    if(pos == string::npos)
        return defaultNamespace;

    return nsLUT[typeName.substr(0, pos)];
}

static string stripNamespace(string typeName) {
    //strip namespace part of string
    //makes "xs:int" into "int", "tns:Foo" into "Foo" etc.
    size_t pos = typeName.find_last_of(':');

    if(pos == string::npos)
        return typeName;
    else
        return typeName.substr(pos + 1, typeName.length() - pos - 1);
}

static FullName toFullName(string typeName, string defaultNamespace = "") {
    //looks up and strips namespace from typeName and builds a FullName of the result
    return FullName(lookupNamespace(typeName, defaultNamespace), stripNamespace(typeName));
}

static DOMElement *getExpectedChildElement(DOMNode *parent, string childName) {
    for(DOMNode *child = parent->getFirstChild(); child; child = child->getNextSibling()) {
        if(child->getNodeType() == DOMNode::ELEMENT_NODE && child->getLocalName() && XercesString(child->getLocalName()) == childName) {
            DOMElement *childElement = dynamic_cast<DOMElement*>(child);
            CHECK(childElement);

            return childElement;
        }
    }

    throw runtime_error((string)XercesString(parent->getLocalName()) + " missing expected child element " + childName);
}

static vector<DOMElement*> getChildElements(DOMElement *parent) {
    vector<DOMElement*> ret;
    
    for(DOMNode *child = parent->getFirstChild(); child; child = child->getNextSibling()) {
        if(child->getNodeType() == DOMNode::ELEMENT_NODE) {
            DOMElement *childElement = dynamic_cast<DOMElement*>(child);
            CHECK(childElement);

            ret.push_back(childElement);
        }
    }

    return ret;
}

static vector<DOMElement*> getChildElementsByTagName(DOMElement *parent, string childName) {
    vector<DOMElement*> childElements = getChildElements(parent);
    vector<DOMElement*> ret;

    for(size_t x = 0; x < childElements.size(); x++) {
        if(childElements[x]->getLocalName() && XercesString(childElements[x]->getLocalName()) == childName) {
            ret.push_back(childElements[x]);
        }
    }

    return ret;
}

static void parseComplexType(DOMElement *element, FullName fullName, Class *cl = NULL);

static void parseSequence(DOMElement *parent, DOMElement *sequence, Class *cl, bool choice = false) {
    //we expect to see a whole bunch of <element>s here
    //if choice is true then this is a choice sequence - every element is optional
    CHECK(parent);
    CHECK(sequence);

    vector<DOMElement*> children = getChildElementsByTagName(sequence, "element");

    //support <sequence> in <choice> by simply recursing
    //simply put this means the <sequence> tags are ignored
    vector<DOMElement*> subSequences = getChildElementsByTagName(sequence, "sequence");

    if(subSequences.size() > 0 && !choice) {
        //pretend nested sequences are choices for now
        //TODO: we should probably parse the upper sequence's minOccurs/maxOccurs
        choice = true;
    }

    children.insert(children.end(), subSequences.begin(), subSequences.end());
    
    for(size_t x = 0; x < children.size(); x++) {
        DOMElement *child = children[x];
            
        int minOccurs = 1;
        int maxOccurs = 1;

        XercesString typeStr("type");
        XercesString minOccursStr("minOccurs");
        XercesString maxOccursStr("maxOccurs");
        string name = fixIdentifier(XercesString(child->getAttribute(XercesString("name"))));

        if(child->hasAttribute(minOccursStr)) {
            stringstream ss;
            ss << XercesString(child->getAttribute(minOccursStr));
            ss >> minOccurs;
        }

        if(child->hasAttribute(maxOccursStr)) {
            XercesString str(child->getAttribute(maxOccursStr));

            if(str == "unbounded")
                maxOccurs = UNBOUNDED;
            else {
                stringstream ss;
                ss << str;
                ss >> maxOccurs;
            }
        }

        //all choice elements are optional
        if(choice)
            minOccurs = 0;

        if(XercesString(child->getLocalName()) == "sequence") {
            //<sequence> in <choice> - insert the <element>s within as if the were in this <choice>
            parseSequence(parent, child, cl, true);
        } else if(child->hasAttribute(typeStr)) {
            //has type == end point - add as member of cl
            Class::Member info;

            info.name = name;
            //assume in same namespace for now
            info.type = toFullName(XercesString(child->getAttribute(typeStr)));
            info.minOccurs = minOccurs;
            info.maxOccurs = maxOccurs;
            info.isAttribute = false;

            cl->addMember(info);
        } else {
            //no type - anonymous subtype
            //generate name
            FullName subName(cl->name.first, cl->name.second + "_" + (string)name);

            //expect <complexType> sub-tag
            parseComplexType(getExpectedChildElement(child, "complexType"), subName);

            Class::Member info;
            info.name = name;
            info.type = subName;
            info.minOccurs = minOccurs;
            info.maxOccurs = maxOccurs;
            info.isAttribute = false;

            cl->addMember(info);
        }
    }

    //handle <choice>:es in <sequence>:es
    //choices can't have choices in them
    if(choice)
        return;

    vector<DOMElement*> choices = getChildElementsByTagName(sequence, "choice");

    for(size_t x = 0; x < choices.size(); x++) {
        DOMElement *choice = choices[x];

        parseSequence(parent, choice, cl, true);
    }
}

static void parseComplexType(DOMElement *element, FullName fullName, Class *cl) {
    //we handle two cases with <complexType>:
    //child is <sequence>
    //child is <complexContent> - expect grandchild <extension>
    CHECK(element);

    //bootstrap Class pointer in case we didn't come from the recursive <extension> call below
    if(!cl)
        cl = addClass(new Class(fullName, Class::COMPLEX_TYPE));
    
    vector<DOMElement*> childElements = getChildElements(element);

    for(size_t x = 0; x < childElements.size(); x++) {
        DOMElement *child = childElements[x];
        XercesString name(child->getLocalName());

        if(name == "sequence") {
            parseSequence(element, child, cl);
        } else if(name == "choice" || name == "all") {
            if(child->hasAttribute(XercesString("minOccurs")) || child->hasAttribute(XercesString("maxOccurs")))
                throw runtime_error("minOccurs/maxOccurs not currently supported in <choice>/<all> types");

            parseSequence(element, child, cl, true);
        } else if(name == "complexContent" || name == "simpleContent") {
            DOMElement *extension = getExpectedChildElement(child, "extension");
            
            if(!extension->hasAttribute(XercesString("base")))
                throw runtime_error("Extension missing expected attribute base");
            
            //set base type and treat the extension as complexType itself
            FullName base = toFullName(XercesString(extension->getAttribute(XercesString("base"))));

            cl->baseType = base;

            parseComplexType(extension, fullName, cl);
        } else if(name == "attribute") {
            bool optional = false;

            if(!child->hasAttribute(XercesString("type")))
                throw runtime_error("<attribute> missing expected attribute 'type'");

            if(!child->hasAttribute(XercesString("name")))
                throw runtime_error("<attribute> missing expected attribute 'name'");

            string attributeName = fixIdentifier(XercesString(child->getAttribute(XercesString("name"))));

            FullName type = toFullName(XercesString(child->getAttribute(XercesString("type"))));

            //check for optional use
            if(child->hasAttribute(XercesString("use")) && XercesString(child->getAttribute(XercesString("use"))) == "optional")
                optional = true;

            Class::Member info;
            info.name = attributeName;
            info.type = type;
            info.isAttribute = true;
            info.minOccurs = optional ? 0 : 1;
            info.maxOccurs = 1;

            cl->addMember(info);
        } else if(name == "attributeGroup") {
            if(!child->hasAttribute(XercesString("ref")))
                throw runtime_error("<attributeGroup> missing expected attribute 'ref'");

            //add group ref
            cl->groups.push_back(toFullName(XercesString(child->getAttribute(XercesString("ref")))));
        } else {
            throw runtime_error("Unknown complexType child of type " + (string)name);
        }
    }
}

static void parseSimpleType(DOMElement *element, FullName fullName) {
    //expect a <restriction> child element
    CHECK(element);

    DOMElement *restriction = getExpectedChildElement(element, "restriction");

    if(!restriction->hasAttribute(XercesString("base")))
        throw runtime_error("simpleType restriction lacks expected attribute 'base'");

    //convert xs:string and the like to their respective FullName
    FullName baseName = toFullName(XercesString(restriction->getAttribute(XercesString("base"))));

    //add class and return
    addClass(new Class(fullName, Class::SIMPLE_TYPE, baseName));
}

static void parseElement(DOMElement *element, string tns) {
    CHECK(element);

    XercesString nodeNs(element->getNamespaceURI());
    XercesString nodeName(element->getLocalName());

    if(nodeNs != XSL || (
            nodeName != "complexType" &&
            nodeName != "element" &&
            nodeName != "simpleType" &&
            nodeName != "attributeGroup"))
        return;

    //<complexType>, <element> or <simpleType>
    //figure out its class name
    XercesString name(element->getAttribute(XercesString("name")));
    FullName fullName(tns, name);

    if(verbose) cerr << "\t" << "new " << nodeName << ": " << fullName.second << endl;

    if(nodeName == "complexType")
        parseComplexType(element, fullName);
    else if(nodeName == "element") {
        //if <element> is missing type, then its type is anonymous
        FullName type;

        if(!element->hasAttribute(XercesString("type"))) {
            //anonymous element type. derive it using expected <complexType>
            type = FullName(tns, fullName.second + "Type");

            parseComplexType(getExpectedChildElement(element, "complexType"), type);
        } else
            type = toFullName(XercesString(element->getAttribute(XercesString("type"))), tns);

        addClass(new Class(fullName, Class::COMPLEX_TYPE, type))->isDocument = true;
    } else if(nodeName == "simpleType") {
        parseSimpleType(element, fullName);
    } else if(nodeName == "attributeGroup") {
        //handle an attributeGroup almost the same way as a complexType
        //we add the dummy Class group to ::groups rather than ::classes
        //this means it won't result in generated code
        //work() will copy the members of referenced groups to the referencing classes
        Class *group = new Class(fullName, Class::COMPLEX_TYPE);

        try {
        parseComplexType(element, fullName, group);
        } catch(...) {
            delete group;
            throw;
        }

        addClass(group, groups);
    }
}

//sets the Class::Member::cl pointer for each member in each class in classMap
static void resolveMemberRefs(map<FullName, Class*>& classMap) {
    for(map<FullName, Class*>::iterator it = classMap.begin(); it != classMap.end(); it++) {
        for(list<Class::Member>::iterator it2 = it->second->members.begin(); it2 != it->second->members.end(); it2++) {
            if(classes.find(it2->type) == classes.end()) {
                if (it2->minOccurs > 0)
                    throw runtime_error("Undefined type " + it2->type.first + ":" + it2->type.second + " in required member " + it2->name + " of " + it->first.first + ":" + it->first.second);

                //allow members with undefined types as long as they're optional or vectors
                if (verbose)
                    cerr << "Optional/vector member " << it2->name << " of " << it->first.first << ":" << it->first.second <<
                            " is of unknown type " << it2->type.first << ":" << it2->type.second << endl << " - ignoring" << endl;

                it2->cl = NULL;
            } else
                it2->cl = classes[it2->type];
        }
    }
}

static void work(string /*outputDir*/, const vector<string>& schemaNames) {
    XercesDOMParser parser;
    parser.setDoNamespaces(true);

    for(size_t x = 0; x < schemaNames.size(); x++) {
        string name = schemaNames[x];
        parser.parse(name.c_str());

        DOMDocument *document = parser.getDocument();

        if(!document)
            throw runtime_error("Failed to parse " + name + " - file does not exist?");

        DOMElement *root = document->getDocumentElement();

        DOMAttr *targetNamespace = root->getAttributeNode(XercesString("targetNamespace"));
        CHECK(targetNamespace);
        string tns = XercesString(targetNamespace->getValue());

        //HACKHACK: we should handle NS lookup properly
        nsLUT["tns"] = tns;

        if(verbose) cerr << "Target namespace: " << tns << endl;

        vector<DOMElement*> elements = getChildElements(root);

        for(size_t x = 0; x < elements.size(); x++)
            parseElement(elements[x], tns);
    }

    if(verbose) cerr << "About to make second pass. Pointing class members to referenced classes, or failing if any undefined classes are encountered." << endl;

    //make second pass through classes and set all member and base class pointers correctly
    //this has the side effect of catching any undefined classes

    //first resolve member references in both ::classes and ::groups
    resolveMemberRefs(classes);
    resolveMemberRefs(groups);

    for(map<FullName, Class*>::iterator it = classes.begin(); it != classes.end(); it++) {
        if(it->second->hasBase()) {
            if(classes.find(it->second->baseType) == classes.end())
                throw runtime_error("Undefined base type " + it->second->baseType.first + ":" + it->second->baseType.second + " of " + it->second->name.first + ":" + it->second->name.second);

            it->second->base = classes[it->second->baseType];
        } else if(it->second->isDocument)
            throw runtime_error("Document without base type!");

        //insert members of any referenced groups as members in this class
        for(list<FullName>::iterator it2 = it->second->groups.begin(); it2 != it->second->groups.end(); it2++) {
            if(groups.find(*it2) == groups.end())
                throw runtime_error("Undefined group " + it2->first + ":" + it2->second + " in " + it->second->name.first + ":" + it->second->name.second);

            //add each member in the group to the current class
            for(list<Class::Member>::iterator it3 = groups[*it2]->members.begin(); it3 != groups[*it2]->members.end(); it3++)
                it->second->members.push_back(*it3);
        }
    }
}

void doPostResolveInits() {
    if(verbose) cerr << "Doing post-resolve work in preparation for writing headers and implementations." << endl;

    for(map<FullName, Class*>::iterator it = classes.begin(); it != classes.end(); it++)
        it->second->doPostResolveInit();
}

/**
 * Reads the entire contents of an std::istream to a std::string.
 */
static string readIstreamToString(istream& is) {
    ostringstream oss;

    copy(istreambuf_iterator<char>(is), istreambuf_iterator<char>(), ostreambuf_iterator<char>(oss));

    return oss.str();
}

static bool files_changed = false;

/**
 * Replaces contents of the file named by originalName with newContents if there is a difference.
 * If not, the file is untouched.
 * The purpose of this is to avoid the original file being marked as changed,
 * so that this tool can be incorporated into an automatic build system where only the files that did change have to be recompiled.
 */
static void diffAndReplace(string fileName, string newContents, bool dry_run) {
    //read contents of the original file. missing files give rise to empty strings
    string originalContents;

    {
        ifstream originalIfs(fileName.c_str());

        originalContents = readIstreamToString(originalIfs);

        //input file gets closed here, so that we can write to it later
    }

    if(newContents == originalContents) {
        //no difference
        if(verbose) cerr << ". " << fileName << endl;
    } else {
        //contents differ - either original does not exist or the schema changed for this type
#ifdef WIN32
        if(_unlink(fileName.c_str())) {
#else
        if(unlink(fileName.c_str())) {
#endif
            //new file added
            cerr << "A " << fileName << endl;
        } else {
            //old file modified (replaced)
            cerr << "M " << fileName << endl;
        }

        files_changed = true;

        if(dry_run)
            return;

        //write new content
        ofstream ofs(fileName.c_str());
        
        ofs << newContents;
    }
}

string generateCMakeLists() {
    ostringstream oss;

    oss << "cmake_minimum_required(VERSION 2.6)" << endl;
    oss << "add_library(" << cmakeTargetName << endl;

    for(map<FullName, Class*>::iterator it = classes.begin(); it != classes.end(); it++)
        if(!it->second->isSimple())
            oss << "\t" << it->first.second << ".cpp" << endl;

    oss << ")" << endl;

    return oss.str();
}

int main_wrapper(int argc, char** argv) {
    try {
        bool dry_run = false;

        if(argc <= 2) {
            printUsage();
            return 1;
        }

        for(; argc > 3; argv++, argc--) {
            if(!strcmp(argv[1], "-v")) {
                verbose = true;
                cerr << "Verbose mode" << endl;

                continue;
            } else if(!strcmp(argv[1], "-d")) {
                generateDefaultCtor = true;
                if(verbose) cerr << "Generating default constructors" << endl;

                continue;
            } else if(!strcmp(argv[1], "-nr")) {
                generateRequiredCtor = false;
                if(verbose) cerr << "Not generating constructors that take the required elements" << endl;

                continue;
            } else if(!strcmp(argv[1], "-nv")) {
                generateRequiredAndVectorsCtor = false;
                if(verbose) cerr << "Not generating constructors that take the required elements and vectors" << endl;

                continue;
            } else if(!strcmp(argv[1], "-a")) {
                generateAllCtor = true;
                if(verbose) cerr << "Generating constructors that take all elements" << endl;

                continue;
            } else if(!strcmp(argv[1], "--namespace")) {
                namespacePrefix = argv[2];
                np = namespacePrefix + "::";
                if(verbose) cerr << "C++ namespace: " << namespacePrefix << endl;

                argv++;
                argc--;

                continue;
            } else if(!strcmp(argv[1], "-cmake")) {
                cmakeTargetName = argv[2];
                if(verbose) cerr << "CMake target name: " << cmakeTargetName << endl;

                argv++;
                argc--;

                continue;
            } else if(!strcmp(argv[1], "--dry-run")) {
                dry_run = true;
                if(verbose) cerr << "Peforming dry run" << endl;

                continue;
            }

            break;
        }

        //sanity check the ctor generation settings
        if(!generateDefaultCtor && !generateRequiredCtor && !generateRequiredAndVectorsCtor) {
            if(!generateAllCtor)
                cerr << "Tried to generate code without any constructors" << endl;
            else
                cerr << "Tried to generate code with only the 'all' constructors, which would make dealing with optional elements too hard" << endl;

            return 1;
        }

        XMLPlatformUtils::Initialize();

        initKeywordSet();

        //HACKHACK: we should handle NS lookup properly
        nsLUT["xs"] = XSL;
        nsLUT["xsl"] = XSL;
        nsLUT["xsd"] = XSL;

        addClass(new ByteClass);
        addClass(new UnsignedByteClass);
        addClass(new ShortClass);
        addClass(new UnsignedShortClass);
        addClass(new IntClass);
        addClass(new UnsignedIntClass);
        addClass(new IntegerClass);
        addClass(new LongClass);
        addClass(new UnsignedLongClass);
        addClass(new StringClass);
        addClass(new AnyURIClass);
        addClass(new FloatClass);
        addClass(new DoubleClass);
        addClass(new TimeClass);
        addClass(new DateClass);
        addClass(new DateTimeClass);
        addClass(new BooleanClass);
        addClass(new LanguageClass);
        addClass(new HexBinaryClass);

        string outputDir = argv[1];
        vector<string> schemaNames;

        for(int x = 2; x < argc; x++)
            schemaNames.push_back(argv[x]);

        work(outputDir, schemaNames);

        doPostResolveInits();

        if(verbose) cerr << "Everything seems to be in order. Writing/updating headers and implementations as needed." << endl;

        //dump the appenders and parsers of all non-build-in classes
        for(map<FullName, Class*>::iterator it = classes.begin(); it != classes.end(); it++) {
            if(!it->second->isBuiltIn()) {
                if(!it->second->isSimple())
                {
                    ostringstream name, implementation;
                    name << outputDir << "/" << it->first.second << ".cpp";

                    //write implementation to memory, then diff against the possibly existing file
                    it->second->writeImplementation(implementation);

                    diffAndReplace(name.str(), implementation.str(), dry_run);
                }

                {
                    ostringstream name, header;
                    name << outputDir << "/" << it->first.second << ".h";

                    //write header to memory, then diff against the possibly existing file
                    it->second->writeHeader(header);

                    diffAndReplace(name.str(), header.str(), dry_run);
                }
            }
        }

        if(cmakeTargetName.size() > 0) {
            ostringstream name;
            name << outputDir << "/CMakeLists.txt";

            diffAndReplace(name.str(), generateCMakeLists(), dry_run);
        }

        XMLPlatformUtils::Terminate();

        if(dry_run) {
            if(files_changed) {
                if(verbose) cerr << "Changes detected" << endl;
                return 1;
            } else
                if(verbose) cerr << "No changes detected" << endl;
        }

        return 0;
    } catch(const std::exception& e) {
        cerr << "Caught exception: " << e.what() << endl;
        return 1;
    } catch(...) {
        cerr << "Caught unknown exception" << endl;
        return 1;
    }
}

int main(int argc, char** argv) {
    int ret = main_wrapper(argc, argv);

    for (map<FullName, Class*>::iterator it = classes.begin(); it != classes.end(); it++)
        delete it->second;

    for (map<FullName, Class*>::iterator it = groups.begin(); it != groups.end(); it++)
        delete it->second;

    return ret;
}
