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
			string getCurrentMillisSuffix = "";
			string getTimeStringSuffix = "";
			string getDateStringSuffix = "";
			string getDateTimeStringSuffix = "";
			string getTimeStringWithFormatSuffix = "";
			string getDateStringWithFormatSuffix = "";
			string getDateTimeStringWithFormatSuffix = "";

			// Define the regex patterns to find Time.getCurrentMillis, Time.getTimeString, Time.getDateString and Time.getDateTimeString.
			string getCurrentMillisPattern = @"Time\.(?i)getCurrentMillis\(\)";
			string getTimeStringPattern = @"Time\.(?i)getTimeString\(\)";
			string getDateStringPattern = @"Time\.(?i)getDateString\(\)";
			string getDateTimeStringPattern = @"Time\.(?i)getDateTimeString\(\)";
			string getTimeStringWithFormatPattern = @"Time\.(?i)getTimeString\(""(?<format>[^""]+)""\)";
			string getDateStringWithFormatPattern = @"Time\.(?i)getDateString\(""(?<format>[^""]+)""\)";
			string getDateTimeStringWithFormatPattern = @"Time\.(?i)getDateTimeString\(""(?<format>[^""]+)""\)";

			// Replace Time.getCurrentMillis with getCurrentMillis and track if found.
			source = Regex.Replace(source, getCurrentMillisPattern, match =>
			{
				foundCurrentMillis = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getCurrentMillis()"))
				{
					getCurrentMillisSuffix = GetRandomMethodIdentifier();
				}
				return $"getCurrentMillis{getCurrentMillisSuffix}()";
			});

			// Replace Time.getTimeString with getTimeString and track if found.
			source = Regex.Replace(source, getTimeStringPattern, match =>
			{
				foundTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getTimeString()"))
				{
					getTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"getTimeString{getTimeStringSuffix}()";
			});

			// Replace Time.getDateString with getDateString and track if found.
			source = Regex.Replace(source, getDateStringPattern, match =>
			{
				foundDateString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getDateString()"))
				{
					getDateStringSuffix = GetRandomMethodIdentifier();
				}
				return $"getDateString{getDateStringSuffix}()";
			});

			// Replace Time.getDateTimeString with getDateTimeString and track if found.
			source = Regex.Replace(source, getDateTimeStringPattern, match =>
			{
				foundDateTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getDateTimeString()"))
				{
					getDateTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"getDateTimeString{getDateTimeStringSuffix}()";
			});

			// Replace Time.getTimeString with getTimeStringWithFormat and track if found.
			source = Regex.Replace(source, getTimeStringWithFormatPattern, match =>
			{
				foundTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getTimeStringWithFormat("))
				{
					getTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"getTimeStringWithFormat{getTimeStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.getDateString with getDateStringWithFormat and track if found.
			source = Regex.Replace(source, getDateStringWithFormatPattern, match =>
			{
				foundDateStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getDateStringWithFormat("))
				{
					getDateStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"getDateStringWithFormat{getDateStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.getDateTimeString with getDateTimeStringWithFormat and track if found.
			source = Regex.Replace(source, getDateTimeStringWithFormatPattern, match =>
			{
				foundDateTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("getDateTimeStringWithFormat("))
				{
					getDateTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"getDateTimeStringWithFormat{getDateTimeStringWithFormatSuffix}(\"{format}\")";
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

			// Append getCurrentMillis method if it was found.
			if (foundCurrentMillis)
			{
				methods.AppendLine($"long getCurrentMillis{getCurrentMillisSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tauto duration = now.time_since_epoch();");
				methods.AppendLine("\treturn std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();");
				methods.AppendLine("}\n");
			}

			// Append getTimeString method if it was found.
			if (foundTimeString)
			{
				methods.AppendLine($"std::string getTimeString{getTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append getDateString method if it was found.
			if (foundDateString)
			{
				methods.AppendLine($"std::string getDateString{getDateStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append getDateTimeString method if it was found.
			if (foundDateTimeString)
			{
				methods.AppendLine($"std::string getDateTimeString{getDateTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d %H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append getTimeStringWithFormat method if it was found.
			if (foundTimeStringWithFormat)
			{
				methods.AppendLine($"std::string getTimeStringWithFormat{getTimeStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append getDateStringWithFormat method if it was found.
			if (foundDateStringWithFormat)
			{
				methods.AppendLine($"std::string getDateStringWithFormat{getDateStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append getDateTimeStringWithFormat method if it was found.
			if (foundDateTimeStringWithFormat)
			{
				methods.AppendLine($"std::string getDateTimeStringWithFormat{getDateTimeStringWithFormatSuffix}(const std::string& format)");
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
