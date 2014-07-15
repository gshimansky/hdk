#ifndef DataType_NODE_H
#define DataType_NODE_H

#include "ASTNode.h"
#include "../visitor/Visitor.h"

namespace SQL_Namespace {
	class  DataType : public ASTNode {
    
public:
    int dataType_Flag;
    double size1;
    double size2;

    /* data types:
    0 CHARACTER                           
    1 VARCHAR                             
    2 NUMERIC                             
    3 DECIMAL                             
    4 INTEGER                             
    5 SMALLINT                            
    6 FLOAT                               
    7 REAL                                
    8 DOUBLE PRECISION                    

    /**< Constructor */
    explicit DataType(int dF) : dataType_Flag(dF), size1(0), size2(0) {}
    DataType(int dF, double d1) : dataType_Flag(dF), size1(d1), size2(0) {}
    DataType(int dF, double d1, double d2) : dataType_Flag(dF), size1(d1), size2(d2) {}
    

    /**< Accepts the given void visitor by calling v.visit(this) */
    void accept(Visitor &v) {
        v.visit(this);
    }
    
	};
}

#endif // COLUMN_DEF_OPT_NODE_H
