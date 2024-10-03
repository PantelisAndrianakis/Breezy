// Author: Pantelis Andrianakis
// Creation Date: October 3rd 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class TimeTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundCurrentMillis = false;
			bool foundTimeString = false;
			bool foundDateString = false;
			bool foundDateTimeString = false;
			bool foundTimeStringWithFormat = false;
			bool foundDateStringWithFormat = false;
			bool foundDateTimeStringWithFormat = false;

			// Support for random method names to avoid conflicts.
			string timeCurrentMillisSuffix = "";
			string timeTimeStringSuffix = "";
			string timeDateStringSuffix = "";
			string timeDateTimeStringSuffix = "";
			string timeTimeStringWithFormatSuffix = "";
			string timeDateStringWithFormatSuffix = "";
			string timeDateTimeStringWithFormatSuffix = "";

			// Define the regex patterns to find Time.currentMillis, Time.timeString, Time.dateString and Time.dateTimeString.
			string currentMillisPattern = @"Time\.(?i)currentMillis\(\)";
			string timeStringPattern = @"Time\.(?i)timeString\(\)";
			string dateStringPattern = @"Time\.(?i)dateString\(\)";
			string dateTimeStringPattern = @"Time\.(?i)dateTimeString\(\)";
			string timeStringWithFormatPattern = @"Time\.(?i)timeString\(""(?<format>[^""]+)""\)";
			string dateStringWithFormatPattern = @"Time\.(?i)dateString\(""(?<format>[^""]+)""\)";
			string dateTimeStringWithFormatPattern = @"Time\.(?i)dateTimeString\(""(?<format>[^""]+)""\)";

			// Replace Time.currentMillis with timeCurrentMillis and track if found.
			source = Regex.Replace(source, currentMillisPattern, match =>
			{
				foundCurrentMillis = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeCurrentMillis()"))
				{
					timeCurrentMillisSuffix = GetRandomMethodIdentifier();
				}
				return $"timeCurrentMillis{timeCurrentMillisSuffix}()";
			});

			// Replace Time.timeString with timeTimeString and track if found.
			source = Regex.Replace(source, timeStringPattern, match =>
			{
				foundTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeTimeString()"))
				{
					timeTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeTimeString{timeTimeStringSuffix}()";
			});

			// Replace Time.dateString with timeDateString and track if found.
			source = Regex.Replace(source, dateStringPattern, match =>
			{
				foundDateString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeDateString()"))
				{
					timeDateStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeDateString{timeDateStringSuffix}()";
			});

			// Replace Time.dateTimeString with timeDateTimeString and track if found.
			source = Regex.Replace(source, dateTimeStringPattern, match =>
			{
				foundDateTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeDateTimeString()"))
				{
					timeDateTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeDateTimeString{timeDateTimeStringSuffix}()";
			});

			// Replace Time.timeString with timeTimeStringWithFormat and track if found.
			source = Regex.Replace(source, timeStringWithFormatPattern, match =>
			{
				foundTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeTimeStringWithFormat("))
				{
					timeTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeTimeStringWithFormat{timeTimeStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.dateString with timeDateStringWithFormat and track if found.
			source = Regex.Replace(source, dateStringWithFormatPattern, match =>
			{
				foundDateStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeDateStringWithFormat("))
				{
					timeDateStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeDateStringWithFormat{timeDateStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.dateTimeString with timeDateTimeStringWithFormat and track if found.
			source = Regex.Replace(source, dateTimeStringWithFormatPattern, match =>
			{
				foundDateTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeDateStringWithFormat("))
				{
					timeDateTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeDateTimeStringWithFormat{timeDateTimeStringWithFormatSuffix}(\"{format}\")";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <regex> import.
			if (foundCurrentMillis || foundTimeString || foundDateString || foundDateTimeString || foundTimeStringWithFormat || foundDateStringWithFormat || foundDateTimeStringWithFormat)
			{
				source = AddInclude(source, "chrono");
				if (foundTimeString || foundDateString || foundDateTimeString || foundTimeStringWithFormat || foundDateStringWithFormat || foundDateTimeStringWithFormat)
				{
					source = AddInclude(source, "ctime");
					source = AddInclude(source, "iomanip");
					source = AddInclude(source, "sstream");
				}
			}

			// Append timeCurrentMillis method if it was found.
			if (foundCurrentMillis)
			{
				methods.AppendLine($"long timeCurrentMillis{timeCurrentMillisSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tauto duration = now.time_since_epoch();");
				methods.AppendLine("\treturn std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();");
				methods.AppendLine("}\n");
			}

			// Append timeTimeString method if it was found.
			if (foundTimeString)
			{
				methods.AppendLine($"std::string timeTimeString{timeTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateString method if it was found.
			if (foundDateString)
			{
				methods.AppendLine($"std::string timeDateString{timeDateStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateTimeString method if it was found.
			if (foundDateTimeString)
			{
				methods.AppendLine($"std::string timeDateTimeString{timeDateTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d %H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeTimeStringWithFormat method if it was found.
			if (foundTimeStringWithFormat)
			{
				methods.AppendLine($"std::string timeTimeStringWithFormat{timeTimeStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateStringWithFormat method if it was found.
			if (foundDateStringWithFormat)
			{
				methods.AppendLine($"std::string timeDateStringWithFormat{timeDateStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateTimeStringWithFormat method if it was found.
			if (foundDateTimeStringWithFormat)
			{
				methods.AppendLine($"std::string timeDateTimeStringWithFormat{timeDateTimeStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
