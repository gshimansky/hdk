/*
 * Copyright 2017 MapD Technologies, Inc.
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
 */

package org.apache.calcite.rel.externalize;

import com.fasterxml.jackson.databind.type.TypeFactory;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableSet;
import com.mapd.calcite.parser.MapDSqlOperatorTable;

import org.apache.calcite.avatica.AvaticaUtils;
import org.apache.calcite.avatica.util.TimeUnitRange;
import org.apache.calcite.plan.RelOptCluster;
import org.apache.calcite.plan.RelTraitSet;
import org.apache.calcite.rel.RelCollation;
import org.apache.calcite.rel.RelCollationImpl;
import org.apache.calcite.rel.RelCollations;
import org.apache.calcite.rel.RelDistribution;
import org.apache.calcite.rel.RelDistributions;
import org.apache.calcite.rel.RelFieldCollation;
import org.apache.calcite.rel.RelInput;
import org.apache.calcite.rel.RelNode;
import org.apache.calcite.rel.core.AggregateCall;
import org.apache.calcite.rel.core.CorrelationId;
import org.apache.calcite.rel.core.TableModify.Operation;
import org.apache.calcite.rel.type.RelDataType;
import org.apache.calcite.rel.type.RelDataTypeFactory;
import org.apache.calcite.rel.type.RelDataTypeField;
import org.apache.calcite.rex.RexBuilder;
import org.apache.calcite.rex.RexCall;
import org.apache.calcite.rex.RexCorrelVariable;
import org.apache.calcite.rex.RexFieldAccess;
import org.apache.calcite.rex.RexFieldCollation;
import org.apache.calcite.rex.RexInputRef;
import org.apache.calcite.rex.RexLiteral;
import org.apache.calcite.rex.RexNode;
import org.apache.calcite.rex.RexOver;
import org.apache.calcite.rex.RexSubQuery;
import org.apache.calcite.rex.RexWindow;
import org.apache.calcite.rex.RexWindowBound;
import org.apache.calcite.sql.JoinType;
import org.apache.calcite.sql.SqlAggFunction;
import org.apache.calcite.sql.SqlFunction;
import org.apache.calcite.sql.SqlIdentifier;
import org.apache.calcite.sql.SqlKind;
import org.apache.calcite.sql.SqlOperator;
import org.apache.calcite.sql.SqlOperatorTable;
import org.apache.calcite.sql.SqlSyntax;
import org.apache.calcite.sql.fun.SqlStdOperatorTable;
import org.apache.calcite.sql.type.SqlTypeName;
import org.apache.calcite.util.ImmutableBitSet;
import org.apache.calcite.util.JsonBuilder;
import org.apache.calcite.util.Util;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Utilities for converting {@link org.apache.calcite.rel.RelNode} into JSON
 * format.
 */
public class MapDRelJson {
  private final Map<String, Constructor> constructorMap =
          new HashMap<String, Constructor>();
  private final JsonBuilder jsonBuilder;

  public static final List<String> PACKAGES = ImmutableList.of("org.apache.calcite.rel.",
          "org.apache.calcite.rel.core.",
          "org.apache.calcite.rel.logical.",
          "org.apache.calcite.adapter.jdbc.",
          "org.apache.calcite.adapter.enumerable.",
          "org.apache.calcite.adapter.jdbc.JdbcRules$");

  public MapDRelJson(JsonBuilder jsonBuilder) {
    this.jsonBuilder = jsonBuilder;
  }

  public RelNode create(Map<String, Object> map) {
    String type = (String) map.get("type");
    Constructor constructor = getConstructor(type);
    try {
      return (RelNode) constructor.newInstance(map);
    } catch (InstantiationException e) {
      throw new RuntimeException("while invoking constructor for type '" + type + "'", e);
    } catch (IllegalAccessException e) {
      throw new RuntimeException("while invoking constructor for type '" + type + "'", e);
    } catch (InvocationTargetException e) {
      throw new RuntimeException("while invoking constructor for type '" + type + "'", e);
    } catch (ClassCastException e) {
      throw new RuntimeException("while invoking constructor for type '" + type + "'", e);
    }
  }

  public Constructor getConstructor(String type) {
    Constructor constructor = constructorMap.get(type);
    if (constructor == null) {
      Class clazz = typeNameToClass(type);
      try {
        // noinspection unchecked
        constructor = clazz.getConstructor(RelInput.class);
      } catch (NoSuchMethodException e) {
        throw new RuntimeException(
                "class does not have required constructor, " + clazz + "(RelInput)");
      }
      constructorMap.put(type, constructor);
    }
    return constructor;
  }

  /**
   * Converts a type name to a class. E.g. {@code getClass("LogicalProject")}
   * returns {@link org.apache.calcite.rel.logical.LogicalProject}.class.
   */
  public Class typeNameToClass(String type) {
    if (!type.contains(".")) {
      for (String package_ : PACKAGES) {
        try {
          return Class.forName(package_ + type);
        } catch (ClassNotFoundException e) {
          // ignore
        }
      }
    }
    try {
      return Class.forName(type);
    } catch (ClassNotFoundException e) {
      throw new RuntimeException("unknown type " + type);
    }
  }

  /**
   * Inverse of {@link #typeNameToClass}.
   */
  public String classToTypeName(Class<? extends RelNode> class_) {
    final String canonicalName = class_.getName();
    for (String package_ : PACKAGES) {
      if (canonicalName.startsWith(package_)) {
        String remaining = canonicalName.substring(package_.length());
        if (remaining.indexOf('.') < 0 && remaining.indexOf('$') < 0) {
          return remaining;
        }
      }
    }
    return canonicalName;
  }

  public Object toJson(RelCollationImpl node) {
    final List<Object> list = new ArrayList<Object>();
    for (RelFieldCollation fieldCollation : node.getFieldCollations()) {
      final Map<String, Object> map = jsonBuilder.map();
      map.put("field", fieldCollation.getFieldIndex());
      map.put("direction", fieldCollation.getDirection().name());
      map.put("nulls", fieldCollation.nullDirection.name());
      list.add(map);
    }
    return list;
  }

  public Object toJson(RexFieldCollation node) {
    final Map<String, Object> map = jsonBuilder.map();
    map.put("field", toJson(node.left));
    map.put("direction", node.getDirection().name());
    map.put("nulls", node.getNullDirection().name());
    return map;
  }

  public RelCollation toCollation(List<Map<String, Object>> jsonFieldCollations) {
    final List<RelFieldCollation> fieldCollations = new ArrayList<RelFieldCollation>();
    for (Map<String, Object> map : jsonFieldCollations) {
      fieldCollations.add(toFieldCollation(map));
    }
    return RelCollations.of(fieldCollations);
  }

  public RelFieldCollation toFieldCollation(Map<String, Object> map) {
    final Integer field = (Integer) map.get("field");
    final RelFieldCollation.Direction direction = Util.enumVal(
            RelFieldCollation.Direction.class, (String) map.get("direction"));
    final RelFieldCollation.NullDirection nullDirection = Util.enumVal(
            RelFieldCollation.NullDirection.class, (String) map.get("nulls"));
    return new RelFieldCollation(field, direction, nullDirection);
  }

  public RelDistribution toDistribution(Object o) {
    return RelDistributions.ANY; // TODO:
  }

  public RelDataType toType(RelDataTypeFactory typeFactory, Object o) {
    if (o instanceof List) {
      @SuppressWarnings("unchecked")
      final List<Map<String, Object>> jsonList = (List<Map<String, Object>>) o;
      final RelDataTypeFactory.Builder builder = typeFactory.builder();
      for (Map<String, Object> jsonMap : jsonList) {
        builder.add((String) jsonMap.get("name"), toType(typeFactory, jsonMap));
      }
      return builder.build();
    } else {
      final Map<String, Object> map = (Map<String, Object>) o;
      final SqlTypeName sqlTypeName =
              Util.enumVal(SqlTypeName.class, (String) map.get("type"));
      final Integer precision = (Integer) map.get("precision");
      final Integer scale = (Integer) map.get("scale");
      final RelDataType type;
      if (precision == null) {
        type = typeFactory.createSqlType(sqlTypeName);
      } else if (scale == null) {
        type = typeFactory.createSqlType(sqlTypeName, precision);
      } else {
        type = typeFactory.createSqlType(sqlTypeName, precision, scale);
      }
      final boolean nullable = (Boolean) map.get("nullable");
      return typeFactory.createTypeWithNullability(type, nullable);
    }
  }

  public Object toJson(AggregateCall node) {
    final Map<String, Object> map = jsonBuilder.map();
    map.put("agg", toJson(node.getAggregation()));
    map.put("type", toJson(node.getType()));
    map.put("distinct", node.isDistinct());
    map.put("operands", node.getArgList());
    return map;
  }

  Object toJson(Object value) {
    if (value == null || value instanceof Number || value instanceof String
            || value instanceof Boolean) {
      return value;
    } else if (value instanceof RexNode) {
      return toJson((RexNode) value);
    } else if (value instanceof CorrelationId) {
      return toJson((CorrelationId) value);
    } else if (value instanceof List) {
      final List<Object> list = jsonBuilder.list();
      for (Object o : (List) value) {
        list.add(toJson(o));
      }
      return list;
    } else if (value instanceof ImmutableBitSet) {
      final List<Object> list = jsonBuilder.list();
      for (Integer integer : (ImmutableBitSet) value) {
        list.add(toJson(integer));
      }
      return list;
    } else if (value instanceof ImmutableSet) {
      final List<Object> list = jsonBuilder.list();
      for (Object element : (ImmutableSet) value) {
        list.add(toJson(element));
      }
      return list;
    } else if (value instanceof AggregateCall) {
      return toJson((AggregateCall) value);
    } else if (value instanceof RelCollationImpl) {
      return toJson((RelCollationImpl) value);
    } else if (value instanceof RexFieldCollation) {
      return toJson((RexFieldCollation) value);
    } else if (value instanceof RelDataType) {
      return toJson((RelDataType) value);
    } else if (value instanceof RelDataTypeField) {
      return toJson((RelDataTypeField) value);
    } else if (value instanceof JoinType) {
      return value.toString();
    } else if (value instanceof Operation) {
      return value.toString();
    } else {
      throw new UnsupportedOperationException("type not serializable: " + value
              + " (type " + value.getClass().getCanonicalName() + ")");
    }
  }

  private Object toJson(RelDataType node) {
    if (node.isStruct()) {
      final List<Object> list = jsonBuilder.list();
      for (RelDataTypeField field : node.getFieldList()) {
        list.add(toJson(field));
      }
      return list;
    } else {
      final Map<String, Object> map = jsonBuilder.map();
      map.put("type", node.getSqlTypeName().name());
      map.put("nullable", node.isNullable());
      if (node.getSqlTypeName().allowsPrec()) {
        map.put("precision", node.getPrecision());
      }
      if (node.getSqlTypeName().allowsScale()) {
        map.put("scale", node.getScale());
      }
      return map;
    }
  }

  private Object toJson(RelDataTypeField node) {
    final Map<String, Object> map = (Map<String, Object>) toJson(node.getType());
    map.put("name", node.getName());
    return map;
  }

  private Object toJson(CorrelationId node) {
    return node.getId();
  }

  private Object toJson(final RexWindowBound window_bound) {
    final Map<String, Object> map = jsonBuilder.map();
    map.put("unbounded", toJson(window_bound.isUnbounded()));
    map.put("preceding", toJson(window_bound.isPreceding()));
    map.put("following", toJson(window_bound.isFollowing()));
    map.put("is_current_row", toJson(window_bound.isCurrentRow()));
    map.put("offset",
            window_bound.getOffset() != null ? toJson(window_bound.getOffset()) : null);
    map.put("order_key", toJson(window_bound.getOrderKey()));
    return map;
  }

  private Object toJson(RexNode node) {
    final Map<String, Object> map;
    switch (node.getKind()) {
      case FIELD_ACCESS:
        map = jsonBuilder.map();
        final RexFieldAccess fieldAccess = (RexFieldAccess) node;
        map.put("field", fieldAccess.getField().getName());
        map.put("expr", toJson(fieldAccess.getReferenceExpr()));
        return map;
      case LITERAL:
        final RexLiteral literal = (RexLiteral) node;
        final Object value2 = literal.getValue2();
        map = jsonBuilder.map();
        if (value2 instanceof TimeUnitRange) {
          map.put("literal", value2.toString());
        } else {
          map.put("literal", value2);
        }
        map.put("type", literal.getTypeName().name());
        map.put("target_type", literal.getType().getSqlTypeName().toString());
        final Object value = literal.getValue();
        if (value instanceof BigDecimal) {
          map.put("scale", ((BigDecimal) value).scale());
          map.put("precision", ((BigDecimal) value).precision());
        } else {
          map.put("scale", literal.getType().getScale());
          map.put("precision", literal.getType().getPrecision());
        }
        map.put("type_scale", literal.getType().getScale());
        map.put("type_precision", literal.getType().getPrecision());
        return map;
      case INPUT_REF:
        map = jsonBuilder.map();
        map.put("input", ((RexInputRef) node).getIndex());
        return map;
      case CORREL_VARIABLE:
        map = jsonBuilder.map();
        map.put("correl", ((RexCorrelVariable) node).getName());
        map.put("type", toJson(node.getType()));
        return map;
      default:
        if (node instanceof RexCall) {
          final RexCall call = (RexCall) node;
          map = jsonBuilder.map();
          map.put("op", toJson(call.getOperator()));
          final List<Object> list = jsonBuilder.list();
          for (RexNode operand : call.getOperands()) {
            list.add(toJson(operand));
          }
          map.put("operands", list);
          map.put("type", toJson(node.getType()));
          if (node instanceof RexSubQuery) {
            final MapDRelJsonWriter subqueryWriter = new MapDRelJsonWriter();
            ((RexSubQuery) node).rel.explain(subqueryWriter);
            map.put("subquery", subqueryWriter.asJsonMap());
          }
          if (node instanceof RexOver) {
            final RexWindow window = ((RexOver) node).getWindow();
            final List<Object> partitionKeyList = jsonBuilder.list();
            for (final RexNode partitionKey : window.partitionKeys) {
              partitionKeyList.add(toJson(partitionKey));
            }
            map.put("partition_keys", partitionKeyList);
            final List<Object> orderKeyList = jsonBuilder.list();
            for (final RexFieldCollation orderKey : window.orderKeys) {
              orderKeyList.add(toJson(orderKey));
            }
            map.put("order_keys", orderKeyList);
            RexWindowBound lower_bound = window.getLowerBound();
            RexWindowBound upper_bound = window.getUpperBound();
            map.put("lower_bound", toJson(lower_bound));
            map.put("upper_bound", toJson(upper_bound));
            map.put("is_rows", toJson(window.isRows()));
          }
          if (call.getOperator() instanceof SqlFunction) {
            switch (((SqlFunction) call.getOperator()).getFunctionType()) {
              case USER_DEFINED_CONSTRUCTOR:
              case USER_DEFINED_FUNCTION:
              case USER_DEFINED_PROCEDURE:
              case USER_DEFINED_SPECIFIC_FUNCTION:
                map.put("class", call.getOperator().getClass().getName());
            }
          }
          return map;
        }
        throw new UnsupportedOperationException("unknown rex " + node);
    }
  }

  RexNode toRex(RelInput relInput, Object o) {
    final RelOptCluster cluster = relInput.getCluster();
    final RexBuilder rexBuilder = cluster.getRexBuilder();
    if (o == null) {
      return null;
    } else if (o instanceof Map) {
      Map map = (Map) o;
      final String op = (String) map.get("op");
      if (op != null) {
        final List operands = (List) map.get("operands");
        final Object jsonType = map.get("type");
        final SqlOperator operator = toOp(op, map);
        final List<RexNode> rexOperands = toRexList(relInput, operands);
        RelDataType type;
        if (jsonType != null) {
          type = toType(cluster.getTypeFactory(), jsonType);
        } else {
          type = rexBuilder.deriveReturnType(operator, rexOperands);
        }
        return rexBuilder.makeCall(type, operator, rexOperands);
      }
      final Integer input = (Integer) map.get("input");
      if (input != null) {
        List<RelNode> inputNodes = relInput.getInputs();
        int i = input;
        for (RelNode inputNode : inputNodes) {
          final RelDataType rowType = inputNode.getRowType();
          if (i < rowType.getFieldCount()) {
            final RelDataTypeField field = rowType.getFieldList().get(i);
            return rexBuilder.makeInputRef(field.getType(), input);
          }
          i -= rowType.getFieldCount();
        }
        throw new RuntimeException("input field " + input + " is out of range");
      }
      final String field = (String) map.get("field");
      if (field != null) {
        final Object jsonExpr = map.get("expr");
        final RexNode expr = toRex(relInput, jsonExpr);
        return rexBuilder.makeFieldAccess(expr, field, true);
      }
      final String correl = (String) map.get("correl");
      if (correl != null) {
        final Object jsonType = map.get("type");
        RelDataType type = toType(cluster.getTypeFactory(), jsonType);
        return rexBuilder.makeCorrel(type, new CorrelationId(correl));
      }
      if (map.containsKey("literal")) {
        final Object literal = map.get("literal");
        final SqlTypeName sqlTypeName =
                Util.enumVal(SqlTypeName.class, (String) map.get("type"));
        if (literal == null) {
          return rexBuilder.makeNullLiteral(
                  cluster.getTypeFactory().createSqlType(sqlTypeName));
        }

        // omnisci serializes numeric literals differently, we need more data
        // then the number. So, we need to have a special case for that.
        if (literal instanceof Number) {
          final SqlTypeName targetTypeName =
                  Util.enumVal(SqlTypeName.class, (String) map.get("target_type"));
          final long scale = ((Number) map.get("scale")).longValue();
          final long precision = ((Number) map.get("precision")).longValue();
          final long typeScale = ((Number) map.get("type_scale")).longValue();
          final long typePrecision = ((Number) map.get("type_precision")).longValue();
          RelDataTypeFactory typeFactory = cluster.getTypeFactory();

          BigDecimal value =
                  BigDecimal.valueOf(((Number) literal).longValue(), (int) scale);

          if (typeScale != 0 && typeScale != -2147483648) {
            return rexBuilder.makeLiteral(value,
                    typeFactory.createSqlType(
                            SqlTypeName.DECIMAL, (int) typePrecision, (int) typeScale),
                    false);
          } else {
            return rexBuilder.makeLiteral(
                    value, typeFactory.createSqlType(targetTypeName), false);
          }
        } else {
          return toRex(relInput, literal);
        }
      }
      throw new UnsupportedOperationException("cannot convert to rex " + o);
    } else if (o instanceof Boolean) {
      return rexBuilder.makeLiteral((Boolean) o);
    } else if (o instanceof String) {
      return rexBuilder.makeLiteral((String) o);
    } else if (o instanceof Number) {
      final Number number = (Number) o;
      if (number instanceof Double || number instanceof Float) {
        return rexBuilder.makeApproxLiteral(BigDecimal.valueOf(number.doubleValue()));
      } else {
        return rexBuilder.makeExactLiteral(BigDecimal.valueOf(number.longValue()));
      }
    } else {
      throw new UnsupportedOperationException("cannot convert to rex " + o);
    }
  }

  private List<RexNode> toRexList(RelInput relInput, List operands) {
    final List<RexNode> list = new ArrayList<RexNode>();
    for (Object operand : operands) {
      list.add(toRex(relInput, operand));
    }
    return list;
  }

  private SqlOperator toOp(String op) {
    return toOp(op, new HashMap<>());
  }

  private SqlOperator toOp(String op, Map<String, Object> map) {
    // TODO: build a map, for more efficient lookup
    // TODO: look up based on SqlKind
    MapDSqlOperatorTable operatorTable =
            new MapDSqlOperatorTable(SqlStdOperatorTable.instance());
    MapDSqlOperatorTable.addUDF(operatorTable, null);
    final List<SqlOperator> operatorList = operatorTable.getOperatorList();
    for (SqlOperator operator : operatorList) {
      if (operator.getName().equals(op)) {
        return operator;
      }
    }
    String class_ = (String) map.get("class");
    if (class_ != null) {
      try {
        return (SqlOperator) Class.forName(class_).getDeclaredConstructor().newInstance();
      } catch (InstantiationException e) {
        throw new RuntimeException(e);
      } catch (IllegalAccessException e) {
        throw new RuntimeException(e);
      } catch (ClassNotFoundException e) {
        throw new RuntimeException(e);
      } catch (NoSuchMethodException e) {
        throw new RuntimeException(e);
      } catch (InvocationTargetException e) {
        throw new RuntimeException(e);
      }
    }
    throw new RuntimeException("Operator " + op + " does not supported");
  }

  SqlOperator toOp(RelInput relInput, String name) {
    // in case different operator has the same kind, check with both name and kind.
    String kind = name;
    String syntax = "FUNCTION";
    SqlKind sqlKind = SqlKind.valueOf(kind);
    SqlSyntax sqlSyntax = SqlSyntax.valueOf(syntax);
    MapDSqlOperatorTable operatorTable =
            new MapDSqlOperatorTable(SqlStdOperatorTable.instance());
    MapDSqlOperatorTable.addUDF(operatorTable, null);
    final List<SqlOperator> operators = operatorTable.getOperatorList();
    List<String> names = new ArrayList<>();
    for (SqlOperator operator : operators) {
      names.add(operator.toString());
      if (operator.getName().equals(name)) {
        return operator;
      }
    }
    throw new RuntimeException("Aggregation function with name " + name
            + " not found, search in " + names.toString());
  }

  SqlAggFunction toAggregation(String agg) {
    return (SqlAggFunction) toOp(agg);
  }

  SqlAggFunction toAggregation(RelInput relInput, String agg) {
    return (SqlAggFunction) toOp(relInput, agg);
  }

  private String toJson(SqlOperator operator) {
    // User-defined operators are not yet handled.
    return operator.getName();
  }
}

// End RelJson.java
