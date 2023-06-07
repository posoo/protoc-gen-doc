/*
  Copyright 2014, 2015, 2016 Elvis Stansvik

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
*/

#include "mustache.h"

#include <algorithm>
#include <iostream>
#include <string>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVariant>
#include <QVariantHash>
#include <QVariantList>

#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/printer.h>

namespace gp = google::protobuf;
namespace ms = Mustache;

/**
 * Context class for the documentation generator.
 */
class DocGeneratorContext {
public:
    QString template_;      /**< Mustache template, or QString() for raw JSON output */
    QString outputFileName; /**< Output filename. */
    bool noExclude;         /**< Ignore @exclude directives? */
    QVariantList files;     /**< List of files to render. */
};

/// Documentation generator context instance.
static DocGeneratorContext generatorContext;

/**
 * Returns the "long" name of the message, enum, field or extension described by
 * @p descriptor.
 *
 * The long name is the name of the message, field, enum or extension, preceeded
 * by the names of its enclosing types, separated by dots. E.g. for "Baz" it could
 * be "Foo.Bar.Baz".
 */
template<typename T>
static QString longName(const T *descriptor)
{
    if (!descriptor) {
        return QString();
    } else if (!descriptor->containing_type()) {
        return QString::fromStdString(descriptor->name());
    }
    return longName(descriptor->containing_type()) + "." +
                QString::fromStdString(descriptor->name());
}

// Specialization for T = FieldDescriptor, since we want to follow extension_scope()
// if it's an extension, not containing_type().
template<>
QString longName(const gp::FieldDescriptor *fieldDescriptor) {
    if (fieldDescriptor->is_extension()) {
        return longName(fieldDescriptor->extension_scope()) + "." +
                QString::fromStdString(fieldDescriptor->name());
    } else {
        return longName(fieldDescriptor->containing_type()) + "." +
                QString::fromStdString(fieldDescriptor->name());
    }
}

/**
 * Returns true if the variant @p v1 is less than @p v2.
 *
 * It is assumed that both variants contain a QVariantHash with either
 * a "message_long_name", a "message_long_name" or a "extension_long_name"
 * key. This comparator is used when sorting the message, enum and
 * extension lists for a file.
 */
static inline bool longNameLessThan(const QVariant &v1, const QVariant &v2)
{
    if (v1.toHash()["message_long_name"].toString() < v2.toHash()["message_long_name"].toString())
        return true;
    if (v1.toHash()["enum_long_name"].toString() < v2.toHash()["enum_long_name"].toString())
        return true;
    return v1.toHash()["extension_long_name"].toString() < v2.toHash()["extension_long_name"].toString();
}

/**
 * Returns the description of the item described by @p descriptor.
 *
 * The item can be a message, enum, enum value, extension, field, service or
 * service method.
 *
 * The description is taken as the leading comments followed by the trailing
 * comments. If present, a single space is removed from the start of each line.
 * Whitespace is trimmed from the final result before it is returned.
 * 
 * If the described item should be excluded from the generated documentation,
 * @p exclude is set to true. Otherwise it is set to false.
 */
template<typename T>
static QString descriptionOf(const T *descriptor, bool &excluded)
{
    QString description;

    gp::SourceLocation sourceLocation;
    descriptor->GetSourceLocation(&sourceLocation);

    // Check for leading documentation comments.
    QString leading = QString::fromStdString(sourceLocation.leading_comments);
    if (leading.startsWith('*') || leading.startsWith('/')) {
        leading = leading.mid(1);
        leading.replace(QRegularExpression("^ ", QRegularExpression::MultilineOption), "");
        description += leading;
    }

    // Check for trailing documentation comments.
    QString trailing = QString::fromStdString(sourceLocation.trailing_comments);
    if (trailing.startsWith('*') || trailing.startsWith('/')) {
        trailing = trailing.mid(1);
        trailing.replace(QRegularExpression("^ ", QRegularExpression::MultilineOption), "");
        description += trailing;
    }

    // Check if item should be excluded.
    description = description.trimmed();
    excluded = false;
    if (description.startsWith("@exclude")) {
        description = description.mid(8);
        excluded = !generatorContext.noExclude;
    }

    return description;
}

/**
 * Returns the description of the file described by @p fileDescriptor.
 *
 * If the first non-whitespace characters in the file is a block of consecutive
 * single-line (///) documentation comments, or a multi-line documentation comment,
 * the contents of that block of comments or comment is taken as the description of
 * the file. If a line inside a multi-line comment starts with "* ", " *" or " * "
 * then that prefix is stripped from the line before it is added to the description.
 *
 * If the file has no description, QString() is returned. If an error occurs,
 * @p error is set to point to an error message and QString() is returned.
 * 
 * If the described file should be excluded from the generated documentation,
 * @p exclude is set to true. Otherwise it is set to false.
 */
static QString descriptionOf(const gp::FileDescriptor *fileDescriptor, std::string *error, bool &excluded)
{
    // Since there's no API in gp::FileDescriptor for getting the "file
    // level" comment, we open the file and extract this out ourselves.

    // Open file.
    const QString fileName = QString::fromStdString(fileDescriptor->name());
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("%1: %2").arg(fileName).arg(file.errorString()).toStdString();
        return QString();
    }

    // Extract the description.
    QTextStream stream(&file);
    QString description;
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        } else if (line.startsWith("///")) {
            while (!stream.atEnd() && line.startsWith("///")) {
                description += line.mid(line.startsWith("/// ") ? 4 : 3) + '\n';
                line = stream.readLine().trimmed();
            }
            description = description.left(description.size() - 1);
        } else if (line.startsWith("/**") && !line.startsWith("/***/")) {
            line = line.mid(2);
            int start, end;
            while ((end = line.indexOf("*/")) == -1) {
                start = 0;
                if (line.startsWith("*")) ++start;
                if (line.startsWith("* ")) ++start;
                description += line.mid(start) + '\n';
                line = stream.readLine().trimmed();
            }
            start = 0;
            if (line.startsWith("*") && !line.startsWith("*/")) ++start;
            if (line.startsWith("* ")) ++start;
            description += line.mid(start, end - start);
        }
        break;
    }

    // Check if the file should be excluded.
    description = description.trimmed();
    excluded = false;
    if (description.startsWith("@exclude")) {
        description = description.mid(8);
        excluded = !generatorContext.noExclude;
    }

    return description;
}

/**
 * Returns the name of the scalar field type @p type.
 */
static QString scalarTypeName(gp::FieldDescriptor::Type type)
{
    switch (type) {
        case gp::FieldDescriptor::TYPE_BOOL:
            return "bool";
        case gp::FieldDescriptor::TYPE_BYTES:
            return "bytes";
        case gp::FieldDescriptor::TYPE_DOUBLE:
            return "double";
        case gp::FieldDescriptor::TYPE_FIXED32:
            return "fixed32";
        case gp::FieldDescriptor::TYPE_FIXED64:
            return "fixed64";
        case gp::FieldDescriptor::TYPE_FLOAT:
            return "float";
        case gp::FieldDescriptor::TYPE_INT32:
            return "int32";
        case gp::FieldDescriptor::TYPE_INT64:
            return "int64";
        case gp::FieldDescriptor::TYPE_SFIXED32:
            return "sfixed32";
        case gp::FieldDescriptor::TYPE_SFIXED64:
            return "sfixed64";
        case gp::FieldDescriptor::TYPE_SINT32:
            return "sint32";
        case gp::FieldDescriptor::TYPE_SINT64:
            return "sint64";
        case gp::FieldDescriptor::TYPE_STRING:
            return "string";
        case gp::FieldDescriptor::TYPE_UINT32:
            return "uint32";
        case gp::FieldDescriptor::TYPE_UINT64:
            return "uint64";
        default:
            return "<unknown>";
    }
}

/**
 * Returns the name of the field label @p label.
 */
static QString labelName(gp::FieldDescriptor::Label label)
{
    switch(label) {
        case gp::FieldDescriptor::LABEL_OPTIONAL:
            return "optional";
        case gp::FieldDescriptor::LABEL_REPEATED:
            return "repeated";
        case gp::FieldDescriptor::LABEL_REQUIRED:
            return "required";
        default:
            return "<unknown>";
    }
}

/**
 * Returns the default value for the field described by @p fieldDescriptor.
 *
 * The field must be of scalar or enum type. If the field has no default value,
 * QString() is returned.
 */
static QString defaultValue(const gp::FieldDescriptor *fieldDescriptor)
{
    if (fieldDescriptor->has_default_value()) {
        switch (fieldDescriptor->cpp_type()) {
            case gp::FieldDescriptor::CPPTYPE_STRING: {
                std::string value = fieldDescriptor->default_value_string();
                if (fieldDescriptor->type() == gp::FieldDescriptor::TYPE_STRING) {
                    return QString("\"%1\"").arg(QString::fromStdString(value));
                } else if (fieldDescriptor->type() == gp::FieldDescriptor::TYPE_BYTES) {
                    return QString("0x%1").arg(QString::fromUtf8(
                                QByteArray(value.c_str()).toHex()));
                } else {
                    return "Unknown";
                }
            }
            case gp::FieldDescriptor::CPPTYPE_BOOL:
                return fieldDescriptor->default_value_bool() ? "true" : "false";
            case gp::FieldDescriptor::CPPTYPE_FLOAT:
                return QString::number(fieldDescriptor->default_value_float());
            case gp::FieldDescriptor::CPPTYPE_DOUBLE:
                return QString::number(fieldDescriptor->default_value_double());
            case gp::FieldDescriptor::CPPTYPE_INT32:
                return QString::number(fieldDescriptor->default_value_int32());
            case gp::FieldDescriptor::CPPTYPE_INT64:
                return QString::number(fieldDescriptor->default_value_int64());
            case gp::FieldDescriptor::CPPTYPE_UINT32:
                return QString::number(fieldDescriptor->default_value_uint32());
            case gp::FieldDescriptor::CPPTYPE_UINT64:
                return QString::number(fieldDescriptor->default_value_uint64());
            case gp::FieldDescriptor::CPPTYPE_ENUM:
                return QString::fromStdString(fieldDescriptor->default_value_enum()->name());
            default:
                return "Unknown";
        }
    } else {
        return QString();
    }
}

/**
 * Add field to variant list.
 *
 * Adds the field described by @p fieldDescriptor to the variant list @p fields.
 */
static void addField(const gp::FieldDescriptor *fieldDescriptor, QVariantList *fields)
{
    bool excluded = false;
    QString description = descriptionOf(fieldDescriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash field;

    // Add basic info.
    field["field_name"] = QString::fromStdString(fieldDescriptor->name());
    field["field_description"] = description;
    field["field_label"] = labelName(fieldDescriptor->label());
    field["field_default_value"] = defaultValue(fieldDescriptor);

    // Add type information.
    gp::FieldDescriptor::Type type = fieldDescriptor->type();
    if (type == gp::FieldDescriptor::TYPE_MESSAGE || type == gp::FieldDescriptor::TYPE_GROUP) {
        // Field is of message / group type.
        const gp::Descriptor *descriptor = fieldDescriptor->message_type();
        field["field_type"] = QString::fromStdString(descriptor->name());
        field["field_long_type"] = longName(descriptor);
        field["field_full_type"] = QString::fromStdString(descriptor->full_name());
    } else if (type == gp::FieldDescriptor::TYPE_ENUM) {
        // Field is of enum type.
        const gp::EnumDescriptor *descriptor = fieldDescriptor->enum_type();
        field["field_type"] = QString::fromStdString(descriptor->name());
        field["field_long_type"] = longName(descriptor);
        field["field_full_type"] = QString::fromStdString(descriptor->full_name());
    } else {
        // Field is of scalar type.
        QString typeName(scalarTypeName(type));
        field["field_type"] = typeName;
        field["field_long_type"] = typeName;
        field["field_full_type"] = typeName;
    }

    fields->append(field);
}

/**
 * Add extension to variant list.
 *
 * Adds the extension described by @p fieldDescriptor to the variant list @p extensions.
 */
static void addExtension(const gp::FieldDescriptor *fieldDescriptor, QVariantList *extensions)
{
    bool excluded = false;
    QString description = descriptionOf(fieldDescriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash extension;

    // Add basic info.
    extension["extension_name"] = QString::fromStdString(fieldDescriptor->name());
    extension["extension_full_name"] = QString::fromStdString(fieldDescriptor->full_name());
    extension["extension_long_name"] = longName(fieldDescriptor);
    extension["extension_description"] = description;
    extension["extension_label"] = labelName(fieldDescriptor->label());
    extension["extension_number"] = QString::number(fieldDescriptor->number());
    extension["extension_default_value"] = defaultValue(fieldDescriptor);

    if (fieldDescriptor->is_extension()) {
        const gp::Descriptor *descriptor = fieldDescriptor->extension_scope();
        if (descriptor != NULL) {
            extension["extension_scope_type"] = QString::fromStdString(descriptor->name());
            extension["extension_scope_long_type"] = longName(descriptor);
            extension["extension_scope_full_type"] = QString::fromStdString(descriptor->full_name());
        }

        descriptor = fieldDescriptor->containing_type();
        if (descriptor != NULL) {
            extension["extension_containing_type"] = QString::fromStdString(descriptor->name());
            extension["extension_containing_long_type"] = longName(descriptor);
            extension["extension_containing_full_type"] = QString::fromStdString(descriptor->full_name());
        }
    }

    // Add type information.
    gp::FieldDescriptor::Type type = fieldDescriptor->type();
    if (type == gp::FieldDescriptor::TYPE_MESSAGE || type == gp::FieldDescriptor::TYPE_GROUP) {
        // Extension is of message / group type.
        const gp::Descriptor *descriptor = fieldDescriptor->message_type();
        extension["extension_type"] = QString::fromStdString(descriptor->name());
        extension["extension_long_type"] = longName(descriptor);
        extension["extension_full_type"] = QString::fromStdString(descriptor->full_name());
    } else if (type == gp::FieldDescriptor::TYPE_ENUM) {
        // Extension is of enum type.
        const gp::EnumDescriptor *descriptor = fieldDescriptor->enum_type();
        extension["extension_type"] = QString::fromStdString(descriptor->name());
        extension["extension_long_type"] = longName(descriptor);
        extension["extension_full_type"] = QString::fromStdString(descriptor->full_name());
    } else {
        // Extension is of scalar type.
        QString typeName(scalarTypeName(type));
        extension["extension_type"] = typeName;
        extension["extension_long_type"] = typeName;
        extension["extension_full_type"] = typeName;
    }

    extensions->append(extension);
}

/**
 * Adds the enum described by @p enumDescriptor to the variant list @p enums.
 */
static void addEnum(const gp::EnumDescriptor *enumDescriptor, QVariantList *enums)
{
    bool excluded = false;
    QString description = descriptionOf(enumDescriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash enum_;

    // Add basic info.
    enum_["enum_name"] = QString::fromStdString(enumDescriptor->name());
    enum_["enum_long_name"] = longName(enumDescriptor);
    enum_["enum_full_name"] = QString::fromStdString(enumDescriptor->full_name());
    enum_["enum_description"] = description;

    // Add enum values.
    QVariantList values;
    for (int i = 0; i < enumDescriptor->value_count(); ++i) {
        const gp::EnumValueDescriptor *valueDescriptor = enumDescriptor->value(i);

        bool excluded = false;
        QString description = descriptionOf(valueDescriptor, excluded);

        if (excluded) {
            continue;
        }

        QVariantHash value;
        value["value_name"] = QString::fromStdString(valueDescriptor->name());
        value["value_number"] = valueDescriptor->number();
        value["value_description"] = description;
        values.append(value);
    }
    enum_["enum_values"] = values;

    enums->append(enum_);
}

/**
 * Add messages to variant list.
 *
 * Adds the message described by @p descriptor and all its nested messages and
 * enums to the variant list @p messages and @p enums, respectively.
 */
static void addMessages(const gp::Descriptor *descriptor,
                        QVariantList *messages,
                        QVariantList *enums)
{
    bool excluded = false;
    QString description = descriptionOf(descriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash message;

    // Add basic info.
    message["message_name"] = QString::fromStdString(descriptor->name());
    message["message_long_name"] = longName(descriptor);
    message["message_full_name"] = QString::fromStdString(descriptor->full_name());
    message["message_description"] = description;

    // Add fields.
    QVariantList fields;
    for (int i = 0; i < descriptor->field_count(); ++i) {
        addField(descriptor->field(i), &fields);
    }
    message["message_fields"] = fields;
    message["message_has_fields"] = !fields.isEmpty();

    // Add nested extensions.
    QVariantList extensions;
    for (int i = 0; i < descriptor->extension_count(); ++i) {
        addExtension(descriptor->extension(i), &extensions);
    }
    message["message_has_extensions"] = !extensions.isEmpty();
    message["message_extensions"] = extensions;

    messages->append(message);

    // Add nested messages and enums.
    for (int i = 0; i < descriptor->nested_type_count(); ++i) {
        addMessages(descriptor->nested_type(i), messages, enums);
    }
    for (int i = 0; i < descriptor->enum_type_count(); ++i) {
        addEnum(descriptor->enum_type(i), enums);
    }
}

/**
 * Add services to variant list.
 *
 * Adds the service described by @p serviceDescriptor and all its methods to the
 * variant list @p services.
 */
static void addService(const gp::ServiceDescriptor *serviceDescriptor, QVariantList *services)
{
    bool excluded = false;
    QString description = descriptionOf(serviceDescriptor, excluded);
    
    if (excluded) {
        return;
    }
    
    QVariantHash service;
    
    // Add basic info.
    service["service_name"] = QString::fromStdString(serviceDescriptor->name());
    service["service_full_name"] = QString::fromStdString(serviceDescriptor->full_name());
    service["service_description"] = description;
    
    // Add methods.
    QVariantList methods;
    for (int i = 0; i < serviceDescriptor->method_count(); ++i) {
        const gp::MethodDescriptor *methodDescriptor = serviceDescriptor->method(i);
        
        bool excluded = false;
        QString description = descriptionOf(methodDescriptor, excluded);
        
        if (excluded) {
            continue;
        }
        
        QVariantHash method;
        method["method_name"] = QString::fromStdString(methodDescriptor->name());
        method["method_description"] = description;
        
        // Add type for method input
        method["method_request_type"] = QString::fromStdString(methodDescriptor->input_type()->name());
        method["method_request_full_type"] = QString::fromStdString(methodDescriptor->input_type()->full_name());
        method["method_request_long_type"] = longName(methodDescriptor->input_type());
        
        // Add type for method output
        method["method_response_type"] = QString::fromStdString(methodDescriptor->output_type()->name());
        method["method_response_full_type"] = QString::fromStdString(methodDescriptor->output_type()->full_name());
        method["method_response_long_type"] = longName(methodDescriptor->output_type());
        
        methods.append(method);
    }
    service["service_methods"] = methods;
    
    services->append(service);
}

/**
 * Add file to variant list.
 *
 * Adds the file described by @p fileDescriptor to the variant list @p files.
 * If an error occurs, @p error is set to point to an error message and the
 * function returns immediately.
 */
static void addFile(const gp::FileDescriptor *fileDescriptor, QVariantList *files, std::string *error)
{
    bool excluded = false;
    QString description = descriptionOf(fileDescriptor, error, excluded);

    if (excluded) {
        return;
    }

    QVariantHash file;

    // Add basic info.
    file["file_name"] = QFileInfo(QString::fromStdString(fileDescriptor->name())).fileName();
    file["file_description"] = description;
    file["file_package"] = QString::fromStdString(fileDescriptor->package());

    QVariantList messages;
    QVariantList enums;
    QVariantList services;
    QVariantList extensions;

    // Add messages.
    for (int i = 0; i < fileDescriptor->message_type_count(); ++i) {
        addMessages(fileDescriptor->message_type(i), &messages, &enums);
    }
    std::sort(messages.begin(), messages.end(), &longNameLessThan);
    file["file_messages"] = messages;

    // Add enums.
    for (int i = 0; i < fileDescriptor->enum_type_count(); ++i) {
        addEnum(fileDescriptor->enum_type(i), &enums);
    }
    std::sort(enums.begin(), enums.end(), &longNameLessThan);
    file["file_enums"] = enums;

    // Add services.
    for (int i = 0; i < fileDescriptor->service_count(); ++i) {
        addService(fileDescriptor->service(i), &services);
    }
    std::sort(services.begin(), services.end(), &longNameLessThan);
    file["file_has_services"] = !services.isEmpty();
    file["file_services"] = services;
    
    // Add file-level extensions
    for (int i = 0; i < fileDescriptor->extension_count(); ++i) {
        addExtension(fileDescriptor->extension(i), &extensions);
    }
    std::sort(extensions.begin(), extensions.end(), &longNameLessThan);
    file["file_has_extensions"] = !extensions.isEmpty();
    file["file_extensions"] = extensions;

    files->append(file);
}

/**
 * Return a formatted template rendering error.
 *
 * @param template_ Template in which the error occurred.
 * @param renderer Template renderer that failed.
 * @return Formatted single-line error.
 */
static std::string formattedError(const QString &template_, const ms::Renderer &renderer)
{
    QString location = template_;
    if (!renderer.errorPartial().isEmpty()) {
        location += " in partial " + renderer.errorPartial();
    }
    return QString("%1:%2: %3")
            .arg(location)
            .arg(renderer.errorPos())
            .arg(renderer.error()).toStdString();
}

/**
 * Returns the list of formats that are supported out of the box.
 */
static QStringList supportedFormats()
{
    QStringList formats;
    QStringList filter = QStringList() << "*.mustache";
    QFileInfoList entries = QDir(":/templates").entryInfoList(filter);
    for (const QFileInfo &entry : entries) {
        formats.append(entry.baseName());
    }
    return formats;
}

/**
 * Returns a usage help string.
 */
static QString usage()
{
    return QString(
        "Usage: --doc_out=%1|<TEMPLATE_FILENAME>,<OUT_FILENAME>[,no-exclude]:<OUT_DIR>")
        .arg(supportedFormats().join("|"));
}

/**
 * Returns the template specified by @p name.
 *
 * The @p name parameter may be either a template file name, or the name of a
 * supported format ("html", "docbook", ...). If an error occured, @p error is
 * set to point to an error message and QString() returned.
 */
static QString readTemplate(const QString &name, std::string *error)
{
    QString builtInFileName = QString(":/templates/%1.mustache").arg(name);
    QString fileName = supportedFormats().contains(name) ? builtInFileName : name;
    QFile file(fileName);

    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("%1: %2").arg(fileName).arg(file.errorString()).toStdString();
        return QString();
    } else {
        return file.readAll();
    }
}

/**
 * Parses the plugin parameter string.
 *
 * @param parameter Plugin parameter string.
 * @param error Pointer to error if parsing failed.
 * @return true on success, otherwise false.
 */
static bool parseParameter(const std::string &parameter, std::string *error)
{
    QStringList tokens = QString::fromStdString(parameter).split(",");

    if (tokens.size() != 2 && tokens.size() != 3) {
        *error = usage().toStdString();
        return false;
    }

    bool noExclude = false;
    if (tokens.size() == 3) {
        if (tokens.at(2) == "no-exclude") {
            noExclude = true;
        } else {
            *error = usage().toStdString();
            return false;
        }
    }

    if (tokens.at(0) != "json") {
        generatorContext.template_ = readTemplate(tokens.at(0), error);
    }
    generatorContext.outputFileName = tokens.at(1);
    generatorContext.noExclude = noExclude;

    return true;
}

/**
 * Template filter for breaking paragraphs into HTML `<p>` elements.
 *
 * Renders @p text with @p renderer in @p context and returns the result with
 * paragraphs enclosed in `<p>..</p>`.
 *
 */
static QString pFilter(const QString &text, ms::Renderer* renderer, ms::Context* context)
{
    QRegularExpression re("(\\n|\\r|\\r\\n)\\s*(\\n|\\r|\\r\\n)");
    return "<p>" + renderer->render(text, context).split(re).join("</p><p>") + "</p>";
}

/**
 * Template filter for breaking paragraphs into DocBook `<para>` elements.
 *
 * Renders @p text with @p renderer in @p context and returns the result with
 * paragraphs enclosed in `<para>..</para>`.
 *
 */
static QString paraFilter(const QString &text, ms::Renderer* renderer, ms::Context* context)
{
    QRegularExpression re("(\\n|\\r|\\r\\n)\\s*(\\n|\\r|\\r\\n)");
    return "<para>" + renderer->render(text, context).split(re).join("</para><para>") + "</para>";
}

/**
 * Template filter for removing line breaks.
 *
 * Renders @p text with @p renderer in @p context and returns the result with
 * all occurrances of `\r\n`, `\n`, `\r` removed in that order.
 */
static QString nobrFilter(const QString &text, ms::Renderer* renderer, ms::Context* context)
{
    QString result = renderer->render(text, context);
    result.remove("\r\n");
    result.remove("\r");
    result.remove("\n");
    return result;
}

/**
 * Renders the list of files.
 *
 * Renders files to the directory specified in @p context. If an error occurred,
 * @p error is set to point to an error message and no output is written.
 *
 * @param context Compiler generator context specifying the output directory.
 * @param error Pointer to error if rendering failed.
 * @return true on success, otherwise false.
 */
static bool render(gp::compiler::GeneratorContext *context, std::string *error)
{
    QVariantHash args;
    QString result;

    if (generatorContext.template_.isEmpty()) {
        // Raw JSON output.
        QJsonDocument document = QJsonDocument::fromVariant(generatorContext.files);
        if (document.isNull()) {
            *error = "Failed to create JSON document";
            return false;
        }
        result = QString(document.toJson());
    } else {
        // Render using template.

        // Add filters.
        args["p"] = QVariant::fromValue(ms::QtVariantContext::fn_t(pFilter));
        args["para"] = QVariant::fromValue(ms::QtVariantContext::fn_t(paraFilter));
        args["nobr"] = QVariant::fromValue(ms::QtVariantContext::fn_t(nobrFilter));

        // Add files list.
        args["files"] = generatorContext.files;

        // Add scalar value types table.
        QString fileName(":/templates/scalar_value_types.json");
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QString("%1: %2").arg(fileName).arg(file.errorString()).toStdString();
            return false;
        }
        QJsonDocument document(QJsonDocument::fromJson(file.readAll()));
        args["scalar_value_types"] = document.array().toVariantList();

        // Render template.
        ms::Renderer renderer;
        ms::QtVariantContext variantContext(args);
        result = renderer.render(generatorContext.template_, &variantContext);

        // Check for errors.
        if (!renderer.error().isEmpty()) {
            *error = formattedError(generatorContext.template_, renderer);
            return false;
        }
    }

    // Write output.
    std::string outputFileName = generatorContext.outputFileName.toStdString();
    gp::io::ZeroCopyOutputStream *stream = context->Open(outputFileName);
    gp::io::Printer printer(stream, '$');
    printer.PrintRaw(result.toStdString());

    return true;
}

/**
 * Documentation generator class.
 */
class DocGenerator : public gp::compiler::CodeGenerator
{
    /// Implements google::protobuf::compiler::CodeGenerator.
    bool Generate(
            const gp::FileDescriptor *fileDescriptor,
            const std::string &parameter,
            gp::compiler::GeneratorContext *context,
            std::string *error) const
    {
        std::vector<const gp::FileDescriptor *> parsedFiles;
        context->ListParsedFiles(&parsedFiles);
        const bool isFirst = fileDescriptor == parsedFiles.front();
        const bool isLast = fileDescriptor == parsedFiles.back();

        if (isFirst) {
            // Parse the plugin parameter.
            if (!parseParameter(parameter, error)) {
                return false;
            }
        }

        // Parse the file.
        addFile(fileDescriptor, &generatorContext.files, error);
        if (!error->empty()) {
            return false;
        }

        if (isLast) {
            // Render output.
            if (!render(context, error)) {
                return false;
            }
        }

        return true;
    }
};

int main(int argc, char *argv[])
{
    // Instantiate and invoke the generator plugin.
    DocGenerator generator;
    return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
