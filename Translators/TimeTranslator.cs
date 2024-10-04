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
			bool foundGetCurrentMillis = false;
			bool foundGetTimeString = false;
			bool foundGetDateString = false;
			bool foundGetDateTimeString = false;
			bool foundGetTimeStringWithFormat = false;
			bool foundGetDateStringWithFormat = false;
			bool foundGetDateTimeStringWithFormat = false;

			// Support for random method names to avoid conflicts.
			string timeGetCurrentMillisSuffix = "";
			string timeGetTimeStringSuffix = "";
			string timeGetDateStringSuffix = "";
			string timeGetDateTimeStringSuffix = "";
			string timeGetTimeStringWithFormatSuffix = "";
			string timeGetDateStringWithFormatSuffix = "";
			string timeGetDateTimeStringWithFormatSuffix = "";

			// Define the regex patterns to find Time.getCurrentMillis, Time.getTimeString, Time.getDateString and Time.getDateTimeString.
			string getCurrentMillisPattern = @"Time\.(?i)getCurrentMillis\(\)";
			string getTimeStringPattern = @"Time\.(?i)getTimeString\(\)";
			string getDateStringPattern = @"Time\.(?i)getDateString\(\)";
			string getDateTimeStringPattern = @"Time\.(?i)getDateTimeString\(\)";
			string getTimeStringWithFormatPattern = @"Time\.(?i)getTimeString\(""(?<format>[^""]+)""\)";
			string getDateStringWithFormatPattern = @"Time\.(?i)getDateString\(""(?<format>[^""]+)""\)";
			string getDateTimeStringWithFormatPattern = @"Time\.(?i)getDateTimeString\(""(?<format>[^""]+)""\)";

			// Replace Time.getCurrentMillis with timeGetCurrentMillis and track if found.
			source = Regex.Replace(source, getCurrentMillisPattern, match =>
			{
				foundGetCurrentMillis = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetCurrentMillis()"))
				{
					timeGetCurrentMillisSuffix = GetRandomMethodIdentifier();
				}
				return $"timeGetCurrentMillis{timeGetCurrentMillisSuffix}()";
			});

			// Replace Time.getTimeString with timeGetTimeString and track if found.
			source = Regex.Replace(source, getTimeStringPattern, match =>
			{
				foundGetTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetTimeString()"))
				{
					timeGetTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeGetTimeString{timeGetTimeStringSuffix}()";
			});

			// Replace Time.getDateString with timeGetDateString and track if found.
			source = Regex.Replace(source, getDateStringPattern, match =>
			{
				foundGetDateString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetDateString()"))
				{
					timeGetDateStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeGetDateString{timeGetDateStringSuffix}()";
			});

			// Replace Time.getDateTimeString with timeGetDateTimeString and track if found.
			source = Regex.Replace(source, getDateTimeStringPattern, match =>
			{
				foundGetDateTimeString = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetDateTimeString()"))
				{
					timeGetDateTimeStringSuffix = GetRandomMethodIdentifier();
				}
				return $"timeGetDateTimeString{timeGetDateTimeStringSuffix}()";
			});

			// Replace Time.getTimeString with timeGetTimeStringWithFormat and track if found.
			source = Regex.Replace(source, getTimeStringWithFormatPattern, match =>
			{
				foundGetTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetTimeStringWithFormat("))
				{
					timeGetTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeGetTimeStringWithFormat{timeGetTimeStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.getDateString with timeGetDateStringWithFormat and track if found.
			source = Regex.Replace(source, getDateStringWithFormatPattern, match =>
			{
				foundGetDateStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetDateStringWithFormat("))
				{
					timeGetDateStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeGetDateStringWithFormat{timeGetDateStringWithFormatSuffix}(\"{format}\")";
			});

			// Replace Time.getDateTimeString with timeGetDateTimeStringWithFormat and track if found.
			source = Regex.Replace(source, getDateTimeStringWithFormatPattern, match =>
			{
				foundGetDateTimeStringWithFormat = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("timeGetDateTimeStringWithFormat("))
				{
					timeGetDateTimeStringWithFormatSuffix = GetRandomMethodIdentifier();
				}
				string format = match.Groups["format"].Value;
				return $"timeGetDateTimeStringWithFormat{timeGetDateTimeStringWithFormatSuffix}(\"{format}\")";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <regex> import.
			if (foundGetCurrentMillis || foundGetTimeString || foundGetDateString || foundGetDateTimeString || foundGetTimeStringWithFormat || foundGetDateStringWithFormat || foundGetDateTimeStringWithFormat)
			{
				source = AddInclude(source, "chrono");
				if (foundGetTimeString || foundGetDateString || foundGetDateTimeString || foundGetTimeStringWithFormat || foundGetDateStringWithFormat || foundGetDateTimeStringWithFormat)
				{
					source = AddInclude(source, "ctime");
					source = AddInclude(source, "iomanip");
					source = AddInclude(source, "sstream");
				}
			}

			// Append timeGetCurrentMillis method if it was found.
			if (foundGetCurrentMillis)
			{
				methods.AppendLine($"long timeGetCurrentMillis{timeGetCurrentMillisSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tauto duration = now.time_since_epoch();");
				methods.AppendLine("\treturn std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();");
				methods.AppendLine("}\n");
			}

			// Append timeGetTimeString method if it was found.
			if (foundGetTimeString)
			{
				methods.AppendLine($"std::string timeGetTimeString{timeGetTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeGetDateString method if it was found.
			if (foundGetDateString)
			{
				methods.AppendLine($"std::string timeGetDateString{timeGetDateStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeGetDateTimeString method if it was found.
			if (foundGetDateTimeString)
			{
				methods.AppendLine($"std::string timeGetDateTimeString{timeGetDateTimeStringSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d %H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeGetTimeStringWithFormat method if it was found.
			if (foundGetTimeStringWithFormat)
			{
				methods.AppendLine($"std::string timeGetTimeStringWithFormat{timeGetTimeStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeGetDateStringWithFormat method if it was found.
			if (foundGetDateStringWithFormat)
			{
				methods.AppendLine($"std::string timeGetDateStringWithFormat{timeGetDateStringWithFormatSuffix}(const std::string& format)");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, format.c_str());");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeGetDateTimeStringWithFormat method if it was found.
			if (foundGetDateTimeStringWithFormat)
			{
				methods.AppendLine($"std::string timeGetDateTimeStringWithFormat{timeGetDateTimeStringWithFormatSuffix}(const std::string& format)");
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
