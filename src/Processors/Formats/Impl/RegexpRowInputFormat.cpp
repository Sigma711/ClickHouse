#include <cstdlib>
#include <base/find_symbols.h>
#include <Processors/Formats/Impl/RegexpRowInputFormat.h>
#include <DataTypes/Serializations/SerializationNullable.h>
#include <Formats/EscapingRuleUtils.h>
#include <Formats/FormatFactory.h>
#include <Formats/SchemaInferenceUtils.h>
#include <IO/ReadHelpers.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}

RegexpFieldExtractor::RegexpFieldExtractor(const FormatSettings & format_settings) : regexp_str(format_settings.regexp.regexp), regexp(regexp_str), skip_unmatched(format_settings.regexp.skip_unmatched)
{
    if (regexp_str.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "The regular expression is not set for the `Regexp` format. It requires setting the value of the `format_regexp` setting.");

    size_t fields_count = regexp.NumberOfCapturingGroups();
    matched_fields.resize(fields_count);
    re2_arguments.resize(fields_count);
    re2_arguments_ptrs.resize(fields_count);
    for (size_t i = 0; i != fields_count; ++i)
    {
        // Bind an argument to a matched field.
        re2_arguments[i] = &matched_fields[i];
        // Save pointer to argument.
        re2_arguments_ptrs[i] = &re2_arguments[i];
    }
}

bool RegexpFieldExtractor::parseRow(PeekableReadBuffer & buf)
{
    PeekableReadBufferCheckpoint checkpoint{buf};

    size_t line_size = 0;

    do
    {
        char * pos = find_first_symbols<'\n'>(buf.position(), buf.buffer().end());
        line_size += pos - buf.position();
        buf.position() = pos;
    } while (buf.position() == buf.buffer().end() && !buf.eof());

    buf.makeContinuousMemoryFromCheckpointToPos();
    buf.rollbackToCheckpoint();

    /// Allow DOS line endings.
    size_t line_to_match = line_size;
    if (line_size > 0 && buf.position()[line_size - 1] == '\r')
        --line_to_match;

    bool match = re2::RE2::FullMatchN(
        re2::StringPiece(buf.position(), line_to_match),
        regexp,
        re2_arguments_ptrs.data(),
        static_cast<int>(re2_arguments_ptrs.size()));

    if (!match && !skip_unmatched)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Line \"{}\" doesn't match the regexp: `{}`",
            std::string(buf.position(), line_to_match), regexp_str);

    buf.position() += line_size;
    if (!buf.eof() && !checkChar('\n', buf))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "No \\n at the end of line.");

    return match;
}

RegexpRowInputFormat::RegexpRowInputFormat(
    ReadBuffer & in_, SharedHeader header_, Params params_, const FormatSettings & format_settings_)
    : RegexpRowInputFormat(std::make_unique<PeekableReadBuffer>(in_), header_, params_, format_settings_)
{
}

RegexpRowInputFormat::RegexpRowInputFormat(
    std::unique_ptr<PeekableReadBuffer> buf_, SharedHeader header_, Params params_, const FormatSettings & format_settings_)
    : IRowInputFormat(header_, *buf_, std::move(params_))
    , buf(std::move(buf_))
    , format_settings(format_settings_)
    , escaping_rule(format_settings_.regexp.escaping_rule)
    , field_extractor(RegexpFieldExtractor(format_settings_))
{
}

void RegexpRowInputFormat::setReadBuffer(ReadBuffer & in_)
{
    buf = std::make_unique<PeekableReadBuffer>(in_);
    IRowInputFormat::setReadBuffer(*buf);
}

void RegexpRowInputFormat::resetReadBuffer()
{
    buf.reset();
    IRowInputFormat::resetReadBuffer();
}

bool RegexpRowInputFormat::readField(size_t index, MutableColumns & columns)
{
    const auto & type = getPort().getHeader().getByPosition(index).type;
    auto matched_field = field_extractor.getField(index);
    ReadBuffer field_buf(const_cast<char *>(matched_field.data()), matched_field.size(), 0);
    try
    {
        return deserializeFieldByEscapingRule(type, serializations[index], *columns[index], field_buf, escaping_rule, format_settings);
    }
    catch (Exception & e)
    {
        e.addMessage("(while reading the value of column " +  getPort().getHeader().getByPosition(index).name + ")");
        throw;
    }
}

void RegexpRowInputFormat::readFieldsFromMatch(MutableColumns & columns, RowReadExtension & ext)
{
    if (field_extractor.getMatchedFieldsSize() != columns.size())
        throw Exception(ErrorCodes::INCORRECT_DATA, "The number of matched fields in line doesn't match the number of columns.");

    ext.read_columns.assign(columns.size(), false);
    for (size_t columns_index = 0; columns_index < columns.size(); ++columns_index)
    {
        ext.read_columns[columns_index] = readField(columns_index, columns);
    }
}

bool RegexpRowInputFormat::readRow(MutableColumns & columns, RowReadExtension & ext)
{
    if (buf->eof())
        return false;

    if (field_extractor.parseRow(*buf))
        readFieldsFromMatch(columns, ext);
    return true;
}

RegexpSchemaReader::RegexpSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_)
    : IRowSchemaReader(
        buf,
        format_settings_,
        getDefaultDataTypeForEscapingRule(format_settings_.regexp.escaping_rule))
    , field_extractor(format_settings)
    , buf(in_)
{
}

std::optional<DataTypes> RegexpSchemaReader::readRowAndGetDataTypes()
{
    if (buf.eof())
        return {};

    field_extractor.parseRow(buf);

    DataTypes data_types;
    data_types.reserve(field_extractor.getMatchedFieldsSize());
    for (size_t i = 0; i != field_extractor.getMatchedFieldsSize(); ++i)
    {
        String field(field_extractor.getField(i));
        data_types.push_back(tryInferDataTypeByEscapingRule(field, format_settings, format_settings.regexp.escaping_rule, &json_inference_info));
    }

    return data_types;
}

void RegexpSchemaReader::transformTypesIfNeeded(DataTypePtr & type, DataTypePtr & new_type)
{
    transformInferredTypesByEscapingRuleIfNeeded(type, new_type, format_settings, format_settings.regexp.escaping_rule, &json_inference_info);
}


void registerInputFormatRegexp(FormatFactory & factory)
{
    factory.registerInputFormat("Regexp", [](
            ReadBuffer & buf,
            const Block & sample,
            IRowInputFormat::Params params,
            const FormatSettings & settings)
    {
        return std::make_shared<RegexpRowInputFormat>(buf, std::make_shared<const Block>(sample), std::move(params), settings);
    });
}

static std::pair<bool, size_t> segmentationEngine(ReadBuffer & in, DB::Memory<> & memory, size_t min_bytes, size_t max_rows)
{
    char * pos = in.position();
    bool need_more_data = true;
    size_t number_of_rows = 0;

    while (loadAtPosition(in, memory, pos) && need_more_data)
    {
        pos = find_first_symbols<'\r', '\n'>(pos, in.buffer().end());
        if (pos > in.buffer().end())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Position in buffer is out of bounds. There must be a bug.");
        if (pos == in.buffer().end())
            continue;

        ++number_of_rows;
        if ((memory.size() + static_cast<size_t>(pos - in.position()) >= min_bytes) || (number_of_rows == max_rows))
            need_more_data = false;

        if (*pos == '\n')
        {
            ++pos;
            if (loadAtPosition(in, memory, pos) && *pos == '\r')
                ++pos;
        }
        else if (*pos == '\r')
        {
            ++pos;
            if (loadAtPosition(in, memory, pos) && *pos == '\n')
                ++pos;
        }
    }

    saveUpToPosition(in, memory, pos);

    return {loadAtPosition(in, memory, pos), number_of_rows};
}

void registerFileSegmentationEngineRegexp(FormatFactory & factory)
{
    factory.registerFileSegmentationEngine("Regexp", &segmentationEngine);
}

void registerRegexpSchemaReader(FormatFactory & factory)
{
    factory.registerSchemaReader("Regexp", [](ReadBuffer & buf, const FormatSettings & settings)
    {
        return std::make_shared<RegexpSchemaReader>(buf, settings);
    });
    factory.registerAdditionalInfoForSchemaCacheGetter("Regexp", [](const FormatSettings & settings)
    {
        auto result = getAdditionalFormatInfoByEscapingRule(settings, settings.regexp.escaping_rule);
        return result + fmt::format(", regexp={}", settings.regexp.regexp);
    });
}

}
