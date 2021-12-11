#include "color.h"
#include "exceptions.h" //@TODO BP REMOVE

Color::Color() {
    memset(RGBA,0,4);
}

/*Color::Color(float r,float g,float b,float a) {
    rgba[0]=r;
    rgba[1]=g;
    rgba[2]=b;
    rgba[3]=a;
}*/

Color::Color(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    RGBA[0] = r;
    RGBA[1] = g;
    RGBA[2] = b;
    RGBA[3] = a;
}

void Color::set(GLubyte r,GLubyte g, GLubyte b, GLubyte a) {
    RGBA[0]=r;
    RGBA[1]=g;
    RGBA[2]=b;
    RGBA[3]=a;
}

// écriture d'une couleur dans un flux
ostream& operator<<(ostream& f,const Color&p) {
    f << "(" << p.RGBA[0] << "," << p.RGBA[1] << "," << p.RGBA[2] << "," << p.RGBA[3] << ")";
    return f;
}

void Color::serialize(std::ofstream &bStream) {
    throw BaseSimulator::NotImplementedException(); // @TODO BP
}

void Color::serialize_cleartext(std::ofstream &dbStream) {
    throw BaseSimulator::NotImplementedException(); // @TODO BP
}

void Color::set(const char *attr) {
    if (attr) {
        string str(attr);
        int pos1 = str.find_first_of(','),
            pos2 = str.find_last_of(',');
        RGBA[0] = stoi(str.substr(0,pos1));
        RGBA[1] = stoi(str.substr(pos1+1,pos2-pos1-1));
        RGBA[2] = stoi(str.substr(pos2+1,str.length()-pos1-1));
    }
}

void Color::glMaterial(GLenum faces,GLenum pName) {
    float tab[4] = { RGBA[0]/255.0f,RGBA[1]/255.0f,RGBA[2]/255.0f,RGBA[3]/255.0f};
    glMaterialfv(faces,pName,tab);
}

string Color::getName() {
    int i=0;
    while (i<NB_COLORS && *this!=Colors[i]) {
        i++;
    }
    string res;
    if (i==NB_COLORS) {
        res = "(" + to_string(RGBA[0]) + "," + to_string(RGBA[1]) + "," + to_string(RGBA[2]) + ")";
    } else {
        res = ColorNames[i];
    }
    return res;
}