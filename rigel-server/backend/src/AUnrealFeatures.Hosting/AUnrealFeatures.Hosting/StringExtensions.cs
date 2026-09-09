using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting
{
    public static class StringExtensions
    {
        public const string CHARSET = "abcdefghijklmnopqrstuvwxyz0123456789";
        public static string CreateRandomString(int length)
        {
            var result = "";
            for (int i = 0; i < length; i++)
                result += CHARSET[Random.Shared.Next(0, CHARSET.Length)];
            return result;
        }
    }
}
